---
sidebar_position: 1
title: "Cookbooks"
sidebar_label: "Cookbooks"
description: "Practical DuckDB AI recipes: enrich tables with LLMs, extract typed records, route rows by decision confidence, evaluate prompts on labeled samples, build semantic search with embeddings, run resumable batch jobs and generate SQL from questions."
keywords: ["DuckDB LLM examples", "AI SQL recipes", "semantic search DuckDB", "text-to-SQL DuckDB", "ai_decide"]
---

# Cookbooks

These cookbooks show practical duckdb-ai workflows. Most of them use the same
`support_tickets` sample table, so you can move from one example to the next
without changing context. A few create their own small tables, so that their
tests have fixed inputs. The production workflows show how
to combine duckdb-ai with DuckDB storage, file, and database extensions.

Each page lists its provider, model and credential prerequisites at the top.

## Start with sample data

- [Create the sample support tickets table](support-ticket-data.md): pasteable
  setup data with text, metadata, numeric, and timestamp columns.

## Production workflows

- [Chain several AI steps in SQL](chain-ai-steps.md): translate, triage and
  summarize in separate saved steps, skip failed rows downstream, and rerun
  without paying twice.
- [Evaluate a prompt or model before a batch run](evaluate-before-batch.md):
  compare candidates on a labeled sample for accuracy, per-label errors,
  confidence thresholds and cost per correct answer.
- [Run production batch enrichment from S3 or Parquet](production-batch-enrichment.md):
  read bounded object-storage inputs, capture row-level failures, and persist
  outputs.
- [Resume a local enrichment job](resumable-enrichment.md): checkpoint bounded
  batches in DuckDB and retry failed rows without repeating saved successes.
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
  tickets by semantic similarity, and store embeddings once for repeated
  searches.
- [Store embeddings in Lance for semantic search](lance-semantic-search.md):
  persist, index, search, and rerank reusable embeddings.
- [Extract typed records from model output](structured-triage-records.md): get
  one typed `STRUCT` per row with `ai_extract_record`.
- [Turn rows into typed decisions (ai_decide)](jev-decisions.md): save choices,
  scores and probabilities as SQL fields, batch rows with TypeSafe, then filter
  or export them.
- [Route rows by decision confidence](decision-routing.md): accept confident
  `ai_decide` answers and send the rest to a chat-model fallback or a review
  table.
- [Generate read-only SQL over local tables](sql-assistant.md): use local schema
  context with `ai_schema_prompt`, `ai_sql`, and `ai_query_data`.

Provider setup is covered separately in the [provider guides](../provider-guides.md).
