#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "executor/spi.h"

#include <curl/curl.h>
#include <string.h>
#include <ctype.h>

PG_MODULE_MAGIC;

/* ---------- EXPORT SYMBOLS ---------- */
PGDLLEXPORT Datum chatbox_query(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum chatbox_explain(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum chatbox_explain_plan(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum chatbox_autopilot(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(chatbox_query);
PG_FUNCTION_INFO_V1(chatbox_explain);
PG_FUNCTION_INFO_V1(chatbox_explain_plan);
PG_FUNCTION_INFO_V1(chatbox_autopilot);

/* ---------- OLLAMA ---------- */
#define OLLAMA_URL "http://localhost:11434/api/generate"
#define OLLAMA_MODEL "mistral"

/* ---------- CURL BUFFER ---------- */
struct buffer
{
    char *data;
    size_t size;
};

static size_t
write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize;
    struct buffer *mem;

    realsize = size * nmemb;
    mem = (struct buffer *) userp;

    mem->data = repalloc(mem->data, mem->size + realsize + 1);
    memcpy(mem->data + mem->size, contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';

    return realsize;
}

/* ---------- JSON ESCAPE ---------- */
static char *
json_escape(const char *src)
{
    StringInfoData buf;
    const char *p;

    initStringInfo(&buf);

    for (p = src; *p; p++)
    {
        if (*p == '"')
            appendStringInfoString(&buf, "\\\"");
        else if (*p == '\\')
            appendStringInfoString(&buf, "\\\\");
        else if (*p == '\n')
            appendStringInfoString(&buf, " ");
        else
            appendStringInfoChar(&buf, *p);
    }

    return buf.data;
}

/* ---------- CALL OLLAMA ---------- */
static char *
ollama_call(const char *prompt)
{
    CURL *curl;
    struct buffer chunk;
    char *escaped;
    char json[8192];

    chunk.data = palloc0(1);
    chunk.size = 0;

    escaped = json_escape(prompt);

    snprintf(json, sizeof(json),
             "{ \"model\": \"%s\", \"prompt\": \"%s\", \"stream\": false }",
             OLLAMA_MODEL, escaped);

    curl = curl_easy_init();
    if (!curl)
        ereport(ERROR, (errmsg("curl init failed")));

    curl_easy_setopt(curl, CURLOPT_URL, OLLAMA_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    if (curl_easy_perform(curl) != CURLE_OK)
        ereport(ERROR, (errmsg("Ollama request failed")));

    curl_easy_cleanup(curl);
    pfree(escaped);

    return chunk.data;
}

/* ---------- EXTRACT SQL ONLY ---------- */
static char *
extract_sql(char *json)
{
    char *p;
    char *out;
    int i;

    p = strstr(json, "\"response\":\"");
    if (!p)
        return pstrdup("SELECT 1;");

    p += strlen("\"response\":\"");
    out = palloc(strlen(p) + 1);

    i = 0;
    while (*p && *p != '"')
    {
        if (*p != '\\')
            out[i++] = *p;
        p++;
    }
    out[i] = '\0';

    return out;
}

/* ---------- SCHEMA CONTEXT ---------- */
static char *
schema_context(void)
{
    int ret;
    StringInfoData ctx;

    initStringInfo(&ctx);

    SPI_connect();

    ret = SPI_exec(
        "SELECT table_name, column_name, data_type "
        "FROM information_schema.columns "
        "WHERE table_schema='public' "
        "ORDER BY table_name", 0);

    if (ret == SPI_OK_SELECT)
    {
        int i;
        for (i = 0; i < SPI_processed; i++)
        {
            HeapTuple tup = SPI_tuptable->vals[i];
            TupleDesc desc = SPI_tuptable->tupdesc;

            appendStringInfo(&ctx, "%s.%s %s; ",
                SPI_getvalue(tup, desc, 1),
                SPI_getvalue(tup, desc, 2),
                SPI_getvalue(tup, desc, 3));
        }
    }

    SPI_finish();
    return ctx.data;
}

/* ---------- chatbox_query ---------- */
Datum
chatbox_query(PG_FUNCTION_ARGS)
{
    char *question;
    char *schema;
    char prompt[8192];
    char *json;
    char *sql;

    question = text_to_cstring(PG_GETARG_TEXT_PP(0));
    schema = schema_context();

    snprintf(prompt, sizeof(prompt),
        "You are PostgreSQL. Using this schema: %s "
        "Write ONLY ONE valid SQL query. No explanation. %s",
        schema, question);

    json = ollama_call(prompt);
    sql = extract_sql(json);

    PG_RETURN_TEXT_P(cstring_to_text(sql));
}

/* ---------- explain ---------- */
Datum
chatbox_explain(PG_FUNCTION_ARGS)
{
    char *sql;
    char prompt[4096];

    sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    snprintf(prompt, sizeof(prompt), "Explain SQL: %s", sql);

    PG_RETURN_TEXT_P(cstring_to_text(extract_sql(ollama_call(prompt))));
}

/* ---------- explain plan ---------- */
Datum
chatbox_explain_plan(PG_FUNCTION_ARGS)
{
    char *sql;
    char prompt[4096];

    sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
    snprintf(prompt, sizeof(prompt), "Explain plan for SQL: %s", sql);

    PG_RETURN_TEXT_P(cstring_to_text(extract_sql(ollama_call(prompt))));
}

/* ---------- autopilot (SAFE SELECT ONLY) ---------- */
Datum
chatbox_autopilot(PG_FUNCTION_ARGS)
{
    char *sql;
    int ret;

    sql = text_to_cstring(PG_GETARG_TEXT_PP(0));

    if (strncasecmp(sql, "select", 6) != 0)
        ereport(ERROR, (errmsg("Unsafe SQL blocked")));

    SPI_connect();
    ret = SPI_execute(sql, true, 5);
    SPI_finish();

    if (ret != SPI_OK_SELECT)
        ereport(ERROR, (errmsg("Execution failed")));

    PG_RETURN_TEXT_P(cstring_to_text("OK"));
}
