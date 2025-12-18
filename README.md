# pg_chatbox
pg_chatbox – Chat with PostgreSQL using Natural Language  pg_chatbox is an open-source PostgreSQL extension that allows users to interact with a PostgreSQL database using natural language queries. It integrates PostgreSQL with open Large Language Models (LLMs) (such as Ollama / Mistral) to convert human-readable questions into SQL.

Features:-

1. Natural Language to SQL
   Ask questions like “list all tables” or “show top 5 largest tables” and get SQL results instantly.

2. Fully integrated with PostgreSQL
   No external application is required; use directly from psql.

3. Supports multiple LLMs
   Works with Ollama-compatible models (both small and large LLMs).

4. Lightweight C extension
   Uses PostgreSQL SPI for safe execution, minimal overhead.

5. Ideal for DBAs and Developers
   Quickly explore database schema, debug queries, or learn SQL.

 How It Works:-
  You submit a natural language query using the function gpt_query().
  The extension sends the query to a configured LLM via HTTP (Ollama or other endpoints).
  The LLM generates SQL based on the database schema.
  SQL is executed inside PostgreSQL.
  Results are returned to you in psql.

  -- List all tables
SELECT gpt_query('List all tables in the database');

-- Find largest tables
SELECT gpt_query('Show top 5 largest tables by size');

-- Check active connections
SELECT gpt_query('How many active connections are there?');


**Installation**
make USE_PGXS=1
make install USE_PGXS=1

Enable the extension in your database:

CREATE EXTENSION pg_chatbox;



**Work in Progress: RAG + LLM Injection**
pg_chatbox is actively being enhanced with Retrieval-Augmented Generation (RAG) features:
This will allow the LLM to use database schema, metadata, and internal knowledge to generate more accurate and context-aware SQL queries.
Users will be able to inject custom prompts or instructions to tailor query generation.
Feature is currently under development and will be released in a future update.

⚠️ **Note: At this stage, RAG + LLM integration is experimental. Early adopters can test basic functionality, but behavior may change in upcoming releases.**
