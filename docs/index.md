---
sidebar_position: 1
slug: /
---

# duckdb-ai documentation

duckdb-ai is a DuckDB extension that lets SQL call AI model providers. It
currently focuses on deterministic request shaping, provider metadata,
completion and embedding calls, structured JSON validation, generated read-only
SQL, and usage logging.

## Installation

The extension is published as a
[DuckDB community extension](https://duckdb.org/community_extensions/extensions/ai):

```sql
INSTALL ai FROM community;
LOAD ai;
```

## Start here

- [SQL function reference](functions.md): every scalar, aggregate, and table
  function exposed by the extension, with examples and result shapes.
- [Cookbooks](cookbooks/index.md): practical workflows over local tables,
  including text enrichment, similarity, Lance-backed semantic search,
  structured records, and SQL generation.
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
