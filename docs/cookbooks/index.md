---
sidebar_position: 1
---

# Cookbooks

These cookbooks show practical duckdb-ai workflows. The introductory examples
use the same `support_tickets` sample data so you can move from one example to
the next without changing context. The production workflows show how to combine
duckdb-ai with DuckDB storage, file, and database extensions.

## Start with sample data

- [Create the sample support tickets table](support-ticket-data.md): pasteable
  setup data with text, metadata, numeric, and timestamp columns.

## Production workflows

- [Run production batch enrichment from S3 or Parquet](production-batch-enrichment.md):
  read bounded object-storage inputs, capture row-level failures, and persist
  outputs.
- [Enrich rows from Postgres or MySQL safely](source-database-enrichment.md):
  attach source databases read-only, materialize local batches, and write only
  to reviewed staging targets.
- [Write audited AI outputs to lakehouse tables](audited-lakehouse-output.md):
  keep run metadata, successful rows, rejected rows, and usage events together.
- [Monitor AI usage, failures, and cost](usage-cost-observability.md): snapshot
  `ai_usage()` for latency, retry, failure, token, and cost reporting.
- [Normalize messy documents into structured records](messy-document-intake.md):
  read JSON, Avro, and Excel inputs before extracting typed records.

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
