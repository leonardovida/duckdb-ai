---
sidebar_position: 10
---

# Write audited AI outputs to lakehouse tables

Use this cookbook when AI output must be queryable later with enough context to
answer: which input row, which model, which run, which prompt contract, which
error, and which usage event produced this result?

The examples use Parquet as the portable baseline and show where to swap in
Delta, Iceberg, or DuckLake tables if your platform already uses them.

## Prerequisites

- Configure a completion provider with a `TYPE duckdb_ai` secret.
- Use a stable run id from the scheduler.
- Decide the output schema before the run starts.

```sql
INSTALL ai FROM community;
LOAD ai;

CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    MODEL 'gpt-4o-mini'
);

SET VARIABLE run_id = 'ticket-audit-2026-07-07T100000Z';
SET VARIABLE prompt_contract_version = 'ticket_triage_v1';
```

## Create a run record

Store the run metadata as data, not only as scheduler logs.

```sql
CREATE OR REPLACE TEMP TABLE ai_run AS
SELECT
    getvariable('run_id') AS run_id,
    getvariable('prompt_contract_version') AS prompt_contract_version,
    'openai' AS provider,
    'gpt-4o-mini' AS model,
    now() AS started_at,
    'support_ticket_triage' AS job_name;
```

## Run enrichment with a stable contract

Keep the prompt contract and row output shape stable across reruns. Use
`ai_try_complete` to preserve row-level failures.

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

## Build audited row tables

Successful rows and rejected rows should have the same identifiers and run
columns so they can be reconciled later.

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
WHERE result.error IS NULL;

CREATE OR REPLACE TEMP TABLE ai_ticket_rejected AS
SELECT
    run_id,
    prompt_contract_version,
    ticket_id,
    customer_id,
    input_hash,
    result.error AS error,
    now() AS loaded_at
FROM ai_attempts
WHERE result.error IS NOT NULL;
```

Capture usage events before calling `ai_clear_usage()`:

```sql
CREATE OR REPLACE TEMP TABLE ai_run_usage AS
SELECT
    getvariable('run_id') AS run_id,
    now() AS captured_at,
    *
FROM ai_usage();
```

## Write Parquet audit datasets

Use separate destinations for run metadata, successful rows, rejected rows, and
usage events. This keeps each table easy to query and replay.

```sql
COPY ai_run
TO 's3://support-prod/ai/audit/runs/run_id=2026-07-07T100000Z/'
(FORMAT parquet, COMPRESSION zstd, OVERWRITE_OR_IGNORE true);

COPY ai_ticket_enriched
TO 's3://support-prod/ai/audit/ticket_enriched/run_id=2026-07-07T100000Z/'
(FORMAT parquet, COMPRESSION zstd, OVERWRITE_OR_IGNORE true);

COPY ai_ticket_rejected
TO 's3://support-prod/ai/audit/ticket_rejected/run_id=2026-07-07T100000Z/'
(FORMAT parquet, COMPRESSION zstd, OVERWRITE_OR_IGNORE true);

COPY ai_run_usage
TO 's3://support-prod/ai/audit/usage/run_id=2026-07-07T100000Z/'
(FORMAT parquet, COMPRESSION zstd, OVERWRITE_OR_IGNORE true);
```

## Load into managed lakehouse tables

If your organization uses Delta, Iceberg, or DuckLake, keep the same audited row
shape and load into managed tables after the temp tables are created.

For Delta tables, attach the table and append reviewed rows:

```sql
INSTALL delta;
LOAD delta;

ATTACH 's3://support-prod/lake/ticket_enriched_delta'
AS ticket_enriched_delta (TYPE delta);

INSERT INTO ticket_enriched_delta
SELECT * FROM ai_ticket_enriched;
```

For an Iceberg REST catalog, attach the catalog first, then use normal SQL:

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

## Learn more

- [DuckDB Delta extension](https://duckdb.org/docs/current/core_extensions/delta)
- [DuckDB Iceberg extension](https://duckdb.org/docs/current/core_extensions/iceberg/overview)
- [Writing Iceberg tables](https://duckdb.org/docs/current/core_extensions/iceberg/writing)
- [DuckLake extension](https://duckdb.org/docs/current/core_extensions/ducklake)
