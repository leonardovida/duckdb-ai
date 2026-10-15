---
sidebar_position: 1
slug: /
title: "LLMs, embeddings and text-to-SQL in DuckDB SQL"
sidebar_label: Overview
description: "Call LLMs from DuckDB SQL with the ai extension: classify, summarize and extract typed data, generate embeddings for semantic search, and ask questions in natural language, with Ollama, OpenAI, Claude, Gemini and 30+ providers."
keywords: ["DuckDB AI", "DuckDB LLM", "LLM in SQL", "DuckDB extension", "DuckDB embeddings", "DuckDB Ollama", "DuckDB OpenAI", "text-to-SQL", "AI SQL functions"]
---

# DuckDB AI: LLMs and embeddings in SQL

duckdb-ai is a DuckDB extension that calls large language models from SQL.
Use it to summarize, classify and filter rows, extract typed fields with a
JSON Schema, generate embeddings for semantic search and RAG, and turn
natural-language questions into read-only SQL. It works with local models
(Ollama, llama.cpp, any OpenAI-compatible server) and hosted providers such as
OpenAI, Anthropic Claude, Google Gemini, Azure OpenAI, Amazon Bedrock,
Databricks and Snowflake Cortex.

| What | Name |
| --- | --- |
| Extension (`INSTALL` / `LOAD`) | `ai` |
| SQL functions | `ai_*` |
| Settings | `duckdb_ai_*` |
| Secret type | `TYPE duckdb_ai` |

## Install

The extension is published as a
[DuckDB community extension](https://duckdb.org/community_extensions/extensions/ai):

```sql
INSTALL ai FROM community;
LOAD ai;
```

These docs describe the source on `main`. The community package can lag behind
it, so check what your installed version provides:

```sql
SELECT function_name
FROM duckdb_functions()
WHERE starts_with(function_name, 'ai_')
ORDER BY function_name;
```

## First query

With [Ollama](https://ollama.com/download) running locally and a model pulled
(`ollama pull qwen3.8:27b`), no API key is needed:

```sql
SET duckdb_ai_provider = 'ollama';
SET duckdb_ai_model = 'qwen3.8:27b';

SELECT ai_complete('Describe DuckDB in one sentence.');

SELECT ticket,
       ai_classify(ticket, ['billing', 'performance', 'other']) AS category
FROM (VALUES ('I was charged twice.'), ('My query got slow.')) AS t(ticket);
```

Without a model setting, `provider := 'ollama'` uses `llama3.2`, so pull that
model or set one as shown. For `ai_decide`, also run `ollama pull nimble`
(Ollama 0.35 or later).

To use a hosted provider, put its API key in an environment variable and
follow the [provider guides](provider-guides.md).

## Find your way around

| If you want to | Read |
| --- | --- |
| Look up a function, option or setting | [SQL function reference](functions.md) |
| Configure a local or hosted model | [Provider guides](provider-guides.md) |
| Copy a working workflow | [Cookbooks](cookbooks/index.md) |
| Get typed answers with confidence from a decision model | [`ai_decide`](functions.md#ai_decidestate-questions-) and [typed decisions](cookbooks/jev-decisions.md) |
| Write SQL for this extension from an agent | [Agent guide](agent-guide.md) |
| Run AI enrichment in production | [Best practices](best-practices.md) |
| Understand retries, caching and concurrency | [Runtime behavior](runtime-behavior.md) |
| Know what data leaves your machine | [Security and data flow](security-data-flow.md) |

Agents and LLM tools can read the whole documentation set as plain text from
[`llms.txt`](https://leonardovida.github.io/duckdb-ai/llms.txt) and
[`llms-full.txt`](https://leonardovida.github.io/duckdb-ai/llms-full.txt).
