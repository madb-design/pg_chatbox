#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include <curl/curl.h>
#include <string.h>

PG_MODULE_MAGIC;

#define OLLAMA_URL "http://localhost:11434/api/generate"
#define OLLAMA_HEADER "Content-Type: application/json"

Datum chatbox_query(PG_FUNCTION_ARGS);
Datum chatbox_explain(PG_FUNCTION_ARGS);
Datum chatbox_explain_plan(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(chatbox_query);
PG_FUNCTION_INFO_V1(chatbox_explain);
PG_FUNCTION_INFO_V1(chatbox_explain_plan);

/* ---------- CURL RESPONSE HANDLER ---------- */
struct response_buffer {
    char *data;
    size_t size;
};

static size_t
write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct response_buffer *mem = (struct response_buffer *) userp;

    mem->data = repalloc(mem->data, mem->size + realsize + 1);
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';

    return realsize;
}

/* ---------- JSON ESCAPE ---------- */
static char *
json_escape(const char *src)
{
    StringInfoData buf;
    initStringInfo(&buf);
    for (const char *p = src; *p; p++)
    {
        switch (*p)
        {
            case '"': appendStringInfoString(&buf, "\\\""); break;
            case '\\': appendStringInfoString(&buf, "\\\\"); break;
            case '\n': appendStringInfoString(&buf, "\\n"); break;
            default: appendStringInfoChar(&buf, *p);
        }
    }
    return buf.data;
}

/* ---------- SPI: FETCH DB SCHEMA ---------- */
static char *
get_schema_of_db(void)
{
    int ret, i;
    TupleDesc tupdesc;
    SPITupleTable *tuptable;
    HeapTuple tuple;

    char *schema = palloc0(8192);

    if ((ret = SPI_connect()) != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("SPI_connect failed")));

    ret = SPI_exec(
        "SELECT table_name || '(' || string_agg(column_name || ' ' || data_type, ', ') || ')' "
        "FROM information_schema.columns "
        "WHERE table_schema NOT IN ('pg_catalog','information_schema') "
        "GROUP BY table_name ORDER BY table_name", 0);

    if (ret != SPI_OK_SELECT)
        ereport(ERROR, (errmsg("SPI_exec failed")));

    tupdesc = SPI_tuptable->tupdesc;
    tuptable = SPI_tuptable;

    for (i = 0; i < SPI_processed; i++)
    {
        tuple = tuptable->vals[i];
        strcat(schema, SPI_getvalue(tuple, tupdesc, 1));
        strcat(schema, "\n");
    }

    SPI_finish();
    return schema;
}

/* ---------- OLLAMA REQUEST ---------- */
static char *
request_ollama(const char *prompt)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    struct response_buffer chunk;

    chunk.data = palloc0(1);
    chunk.size = 0;

    curl = curl_easy_init();
    if (!curl)
        ereport(ERROR, (errmsg("Failed to init curl")));

    char *escaped_prompt = json_escape(prompt);

    char json[8192];
    snprintf(json, sizeof(json),
        "{ \"model\": \"mistral\", \"prompt\": \"%s\", \"stream\": false }",
        escaped_prompt);

    pfree(escaped_prompt);

    headers = curl_slist_append(headers, OLLAMA_HEADER);

    curl_easy_setopt(curl, CURLOPT_URL, OLLAMA_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 400L);

    res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK)
        ereport(ERROR,
                (errmsg("Ollama request failed: %s", curl_easy_strerror(res))));

    return chunk.data;
}

/* ---------- PARSE AND CLEAN OLLAMA RESPONSE ---------- */
static char *
extract_response_text(char *json)
{
    char *start = strstr(json, "\"response\":\"");
    if (!start)
        return "No response from Ollama";

    start += strlen("\"response\":\"");
    char *end = strchr(start, '"');
    if (end)
        *end = '\0';

    // Replace literal \n with actual newlines
    for (char *p = start; *p; p++)
    {
        if (*p == '\\' && *(p+1) == 'n')
        {
            *p = '\n';
            memmove(p+1, p+2, strlen(p+2)+1);
        }
    }

    // Remove Markdown backticks
    char *cleaned = pstrdup(start);
    char *ptr;
    while ((ptr = strstr(cleaned, "```")) != NULL)
        memmove(ptr, ptr + 3, strlen(ptr + 3) + 1);

    // Remove explanations after SQL
    if ((ptr = strstr(cleaned, "The first part")) != NULL)
        *ptr = '\0';
    else if ((ptr = strstr(cleaned, "This query")) != NULL)
        *ptr = '\0';
    else if ((ptr = strstr(cleaned, "Here is the SQL query")) != NULL)
        *ptr = '\0';

    // Skip any leading non-SQL characters
    while (*cleaned && !((*cleaned == 'S' && strncmp(cleaned, "SELECT", 6) == 0) ||
                         (*cleaned == 'W' && strncmp(cleaned, "WITH", 4) == 0) ||
                         (*cleaned == 'I' && strncmp(cleaned, "INSERT", 6) == 0) ||
                         (*cleaned == 'U' && strncmp(cleaned, "UPDATE", 6) == 0) ||
                         (*cleaned == 'D' && strncmp(cleaned, "DELETE", 6) == 0)))
    {
        cleaned++;
    }

    return cleaned;
}

/* ---------- SQL GENERATION ---------- */
Datum
chatbox_query(PG_FUNCTION_ARGS)
{
    char *question = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *schema = get_schema_of_db();

    char prompt[8192];
    snprintf(prompt, sizeof(prompt),
        "You are a PostgreSQL expert.\n"
        "Use ONLY the tables and columns listed below.\n"
        "Do NOT invent tables or columns.\n"
        "Return ONLY the SQL query. Do NOT add any explanations.\n"
        "If you cannot generate the query, return: CANNOT ANSWER.\n\n"
        "Database objects:\n%s\n"
        "Question:\n%s",
        schema, question);

    char *json = request_ollama(prompt);
    char *answer = extract_response_text(json);

    PG_RETURN_TEXT_P(cstring_to_text(answer));
}

/* ---------- EXPLAIN SQL ---------- */
Datum
chatbox_explain(PG_FUNCTION_ARGS)
{
    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));

    char prompt[4096];
    snprintf(prompt, sizeof(prompt),
        "Explain this PostgreSQL query in simple plain text (no Markdown):\n%s", sql);

    char *json = request_ollama(prompt);
    char *answer = extract_response_text(json);

    PG_RETURN_TEXT_P(cstring_to_text(answer));
}

/* ---------- EXPLAIN PLAN ---------- */
Datum
chatbox_explain_plan(PG_FUNCTION_ARGS)
{
    char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));

    char prompt[4096];
    snprintf(prompt, sizeof(prompt),
        "Explain the execution plan for this PostgreSQL query in plain text:\n%s", sql);

    char *json = request_ollama(prompt);
    char *answer = extract_response_text(json);

    PG_RETURN_TEXT_P(cstring_to_text(answer));
}
