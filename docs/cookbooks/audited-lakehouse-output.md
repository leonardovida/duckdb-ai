---
sidebar_position: 8
title: "Write audited AI outputs to lakehouse tables"
sidebar_label: "Audited lakehouse output"
description: "Write LLM outputs from DuckDB to Parquet, Delta, Iceberg or DuckLake tables with run metadata, rejected rows and usage events for auditing."
keywords: ["LLM audit trail", "DuckDB lakehouse", "Delta Iceberg DuckLake", "AI run metadata"]
---

# Write audited AI outputs to lakehouse tables

Use this cookbook when AI output must be queryable later with enough context to
answer: which input row, which model, which run, which prompt contract, which
error, and which usage event produced this result?

The examples use Parquet as the portable baseline and show where to swap in
Delta, Iceberg, or DuckLake tables if your platform already uses them.

## Prerequisites

- Set `OPENAI_API_KEY` in the environment before starting DuckDB. The
  `openai_ai` secret below stores the provider and model, not the key. See the
  [provider guides](../provider-guides.md).
- Create the [sample `support_tickets` table](support-ticket-data.md). The
  examples read its `ticket_id`, `customer_id`, `subject` and `body` columns.
- Use a stable run id from the scheduler.
- Decide the output schema before the run starts.

```sql
INSTALL ai FROM community;
LOAD ai;
INSTALL json;
LOAD json;

CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    MODEL 'gpt-5.6-luna'
);

SET VARIABLE run_id = 'ticket-audit-2026-07-07T100000Z';
SET VARIABLE prompt_contract_version = 'ticket_triage_v1';

-- Fill ai_usage().estimated_cost_usd from the built-in price list.
SET duckdb_ai_use_builtin_model_prices = true;
```

## Create a run record

Store the run metadata as data, not only as scheduler logs. Then clear the usage
buffer, so the usage events you capture later belong to this run only.

```sql
CREATE OR REPLACE TEMP TABLE ai_run AS
SELECT
    getvariable('run_id') AS run_id,
    getvariable('prompt_contract_version') AS prompt_contract_version,
    'openai' AS provider,
    'gpt-5.6-luna' AS model,
    now() AS started_at,
    'support_ticket_triage' AS job_name;

SELECT * FROM ai_clear_usage();
```

## Run enrichment with a stable contract

Keep the prompt contract and row output shape stable across reruns. Use
`ai_try_complete` to preserve row-level failures. `result` is a `STRUCT` with
`response` and `error`.

```sql
CREATE OR REPLACE TEMP TABLE ai_attempts AS
SELECT
    getvariable('run_id') AS run_id,
    getvariable('prompt_contract_version') AS prompt_contract_version,
    ticket_id,
    customer_id,
    subject,
    body,
    md5(subject || chr(10) || body) AS input_hash,
    ai_try_complete(
        'Return JSON with keys summary, product_area, next_action, risk_level. '
        || 'risk_level must be low, medium, or high.'
        || chr(10) || 'Subject: ' || subject
        || chr(10) || 'Body: ' || body,
        secret := 'openai_ai',
        response_format := 'json_object',
        retry_count := 2,
        retry_backoff_ms := 1000
    ) AS result
FROM support_tickets;
```

`risk_level` is a fixed scale. With a decision provider, you can skip the JSON
parsing for that field:
`ai_decide(body, {risk_level: ['low', 'medium', 'high']}, provider := 'ollama', on_error := 'null')`
returns `risk_level DOUBLE` (0 to 2) and `risk_level_confidence`. `ai_decide`
has no capture mode, so treat a NULL result as rejected. See
[route rows by decision confidence](decision-routing.md). Keep
`ai_try_complete` for free-text fields.

## Build audited row tables

Successful rows and rejected rows should have the same identifiers and run
columns so they can be reconciled later.

`response_format := 'json_object'` asks for JSON but does not check the reply.
A reply that is not valid JSON has `error = NULL`, and casting it to `JSON`
fails the whole statement. The `json_valid` filters below route such rows to
the rejected table, so every input row lands in exactly one of the two tables.

```sql
CREATE OR REPLACE TEMP TABLE ai_ticket_enriched AS
SELECT
    run_id,
    prompt_contract_version,
    ticket_id,
    customer_id,
    input_hash,
    result.response::JSON AS response_json,
    result.response::JSON->>'summary' AS summary,
    result.response::JSON->>'product_area' AS product_area,
    result.response::JSON->>'next_action' AS next_action,
    result.response::JSON->>'risk_level' AS risk_level,
    now() AS loaded_at
FROM ai_attempts
WHERE result.error IS NULL
  AND json_valid(result.response);

CREATE OR REPLACE TEMP TABLE ai_ticket_rejected AS
SELECT
    run_id,
    prompt_contract_version,
    ticket_id,
    customer_id,
    input_hash,
    coalesce(result.error, 'missing or invalid JSON response') AS error,
    now() AS loaded_at
FROM ai_attempts
WHERE result.error IS NOT NULL
   OR NOT coalesce(json_valid(result.response), false);
```

Capture usage events before calling `ai_clear_usage()` again. `ai_usage()`
keeps only the latest 1,024 events, so check `dropped_events` first. If it is
above 0, the snapshot is incomplete: run smaller batches and snapshot after
each one. `ai_usage_totals()` keeps its counters even when events are dropped,
so store it with the run.

```sql
CREATE OR REPLACE TEMP TABLE ai_run_usage AS
SELECT
    getvariable('run_id') AS run_id,
    now() AS captured_at,
    *
FROM ai_usage();

SELECT max(dropped_events) AS dropped_events
FROM ai_usage_summary();

CREATE OR REPLACE TEMP TABLE ai_run_usage_totals AS
SELECT
    getvariable('run_id') AS run_id,
    now() AS captured_at,
    *
FROM ai_usage_totals();
```

## Write Parquet audit datasets

Use separate destinations for run metadata, successful rows, rejected rows, and
usage events. This keeps each table easy to query and replay.
`PARTITION_BY (run_id)` writes each run to its own `run_id=<value>/` folder.

```sql
COPY ai_run
TO 's3://support-prod/ai/audit/runs'
(FORMAT parquet, COMPRESSION zstd, PARTITION_BY (run_id), OVERWRITE_OR_IGNORE true);

COPY ai_ticket_enriched
TO 's3://support-prod/ai/audit/ticket_enriched'
(FORMAT parquet, COMPRESSION zstd, PARTITION_BY (run_id), OVERWRITE_OR_IGNORE true);

COPY ai_ticket_rejected
TO 's3://support-prod/ai/audit/ticket_rejected'
(FORMAT parquet, COMPRESSION zstd, PARTITION_BY (run_id), OVERWRITE_OR_IGNORE true);

COPY ai_run_usage
TO 's3://support-prod/ai/audit/usage'
(FORMAT parquet, COMPRESSION zstd, PARTITION_BY (run_id), OVERWRITE_OR_IGNORE true);

COPY ai_run_usage_totals
TO 's3://support-prod/ai/audit/usage_totals'
(FORMAT parquet, PARTITION_BY (run_id), OVERWRITE_OR_IGNORE true);
```

## Load into managed lakehouse tables

If your organization uses Delta, Iceberg, or DuckLake, keep the same audited row
shape and load into managed tables after the temp tables are created.

For Delta tables, attach the table and append reviewed rows. The Delta table
must already exist with the same columns as `ai_ticket_enriched`. This example
appends to it and does not create it.

```sql
INSTALL delta;
LOAD delta;

ATTACH 's3://support-prod/lake/ticket_enriched_delta'
AS ticket_enriched_delta (TYPE delta);

INSERT INTO ticket_enriched_delta
SELECT * FROM ai_ticket_enriched;
```

For an Iceberg REST catalog, attach the catalog first, then use normal SQL.
The `ATTACH` line is a placeholder: replace it with your catalog's `ATTACH`
statement and remove the comment marker, or the statements after it fail
because `iceberg_prod` does not exist.

```sql
INSTALL iceberg;
LOAD iceberg;

-- ATTACH '<catalog-uri>' AS iceberg_prod (TYPE iceberg, ...);

CREATE SCHEMA IF NOT EXISTS iceberg_prod.ai;
CREATE TABLE IF NOT EXISTS iceberg_prod.ai.ticket_enriched AS
SELECT * FROM ai_ticket_enriched
LIMIT 0;

INSERT INTO iceberg_prod.ai.ticket_enriched
SELECT * FROM ai_ticket_enriched;
```

For DuckLake, attach the catalog and data path used by your deployment:

```sql
INSTALL ducklake;
LOAD ducklake;

ATTACH 'ducklake:metadata.ducklake' AS ai_lake
(DATA_PATH 's3://support-prod/ducklake-data');

CREATE SCHEMA IF NOT EXISTS ai_lake.ai;
CREATE TABLE IF NOT EXISTS ai_lake.ai.ticket_enriched AS
SELECT * FROM ai_ticket_enriched
LIMIT 0;

INSERT INTO ai_lake.ai.ticket_enriched
SELECT * FROM ai_ticket_enriched;
```

## Validate the run

Use simple reconciliation checks before publishing downstream tables:

```sql
SELECT 'input' AS table_name, count(*) AS rows FROM ai_attempts
UNION ALL
SELECT 'success', count(*) FROM ai_ticket_enriched
UNION ALL
SELECT 'rejected', count(*) FROM ai_ticket_rejected;

SELECT
    provider,
    model,
    status,
    count(*) AS calls,
    sum(total_tokens) AS total_tokens,
    sum(estimated_cost_usd) AS estimated_cost_usd
FROM ai_run_usage
GROUP BY ALL
ORDER BY estimated_cost_usd DESC NULLS LAST;
```

## Related cookbooks

- [Run production batch enrichment from S3 or Parquet](production-batch-enrichment.md)
- [Enrich rows from Postgres or MySQL](source-database-enrichment.md)
- [Monitor AI usage, failures, and cost](usage-cost-observability.md), which
  reconciles these tables against usage events
- [Normalize messy documents](messy-document-intake.md)

## Learn more

- [DuckDB Delta extension](https://duckdb.org/docs/current/core_extensions/delta)
- [DuckDB Iceberg extension](https://duckdb.org/docs/current/core_extensions/iceberg/overview)
- [Writing Iceberg tables](https://duckdb.org/docs/current/core_extensions/iceberg/writing)
- [DuckLake extension](https://duckdb.org/docs/current/core_extensions/ducklake)
