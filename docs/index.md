---
sidebar_position: 1
slug: /
title: DuckDB AI extension — LLMs, embeddings, and structured extraction in SQL
description: Run local or hosted AI models from DuckDB SQL. Start with agent integration, provider setup, embeddings, JSON extraction, and text-to-SQL examples.
---

# DuckDB AI: LLMs and embeddings in SQL

duckdb-ai adds AI functions to DuckDB SQL for text summarization, classification,
structured JSON extraction, embeddings, semantic search preparation, and
text-to-SQL. Use local Ollama or llama.cpp models, hosted APIs such as OpenAI,
Claude and Gemini, or enterprise gateways such as Databricks and Snowflake Cortex.

The extension is named `ai`; its functions use `ai_*` and its settings use
`duckdb_ai_*`. Check the installed version before using source-only features.

## Installation

The extension is published as a
[DuckDB community extension](https://duckdb.org/community_extensions/extensions/ai):

```sql
INSTALL ai FROM community;
LOAD ai;
```

## Start here

- [Agent integration guide](agent-guide.md): discover the installed API, configure
  credentials, preview requests, and verify behavior with local mocks.
- [SQL function reference](functions.md): every scalar, aggregate, and table
  function exposed by the extension, with examples and result shapes.
- [Cookbooks](cookbooks/index.md): practical workflows over local tables and
  production inputs, including batch enrichment, source database enrichment,
  audited outputs, observability, document intake, similarity, Lance-backed
  semantic search, structured records, and SQL generation.
- [Provider guides](provider-guides.md): end-to-end examples for Ollama, OpenAI,
  Azure OpenAI, Claude, Gemini, Mistral, Z.ai, DeepSeek, OpenRouter,
  Databricks, Snowflake Cortex REST, OpenAI Privacy Filter, and local
  OpenAI-compatible gateways.
- [Best practices](best-practices.md): provider selection, secrets, model
  defaults, structured output, SQL safety, redaction, logging, throughput, cost,
  and release validation guidance.
- [Runtime behavior](runtime-behavior.md): function stability, per-database
  runtime state, response caching, provider concurrency, cancellation, retries,
  egress allowlisting, and JSON parsing.
- [Security and data flow](security-data-flow.md): egress controls, per-function
  data flow, logging defaults, proxy/TLS notes, and vulnerability reporting.
