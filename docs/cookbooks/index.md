---
sidebar_position: 1
---

# Cookbooks

These cookbooks show practical duckdb-ai workflows over local DuckDB tables.
They use the same `support_tickets` sample data so you can move from one example
to the next without changing context.

## Start with sample data

- [Create the sample support tickets table](support-ticket-data.md): pasteable
  setup data with text, metadata, numeric, and timestamp columns.

## Try common workflows

- [Enrich support tickets with AI text functions](support-ticket-enrichment.md):
  summarize, classify, filter, extract, redact, and translate table columns.
- [Compare support tickets with embeddings](support-ticket-similarity.md): rank
  tickets by semantic similarity.
- [Store embeddings in Lance for semantic search](lance-semantic-search.md):
  persist, index, search, and rerank reusable embeddings.
- [Extract typed records from model output](structured-triage-records.md): project
  structured JSON into DuckDB columns.
- [Generate read-only SQL over local tables](sql-assistant.md): use local schema
  context with `ai_schema_prompt`, `ai_sql`, and `ai_query_data`.

Provider setup is covered separately in the [provider guides](../provider-guides.md).
