---
sidebar_position: 7
title: "Enrich rows from Postgres or MySQL safely"
sidebar_label: "Postgres and MySQL enrichment"
description: "Enrich Postgres or MySQL rows with LLMs from DuckDB safely: attach read-only, materialize local batches and write to reviewed staging tables."
keywords: ["DuckDB Postgres LLM", "MySQL enrichment", "read-only ATTACH", "ai_try_complete"]
---

# Enrich rows from Postgres or MySQL safely

Use this cookbook when production rows live in an OLTP database. The safe
pattern is to attach the source read-only, materialize a bounded local working
set, run AI enrichment locally, and write results to an explicit staging target.

## Prerequisites

- Use read-only database credentials for the source attachment.
- Set `OPENAI_API_KEY` in the environment before starting DuckDB. The
  `openai_ai` secret below stores the provider and model, not the key. See the
  [provider guides](../provider-guides.md).
- Configure object-storage credentials if the enriched output will be exported to
  S3-compatible storage.
- Decide whether enriched rows should be exported to files or inserted into a
  separate staging table in the source database.

```sql
INSTALL ai FROM community;
LOAD ai;
INSTALL json;
LOAD json;

INSTALL postgres;
LOAD postgres;
INSTALL mysql;
LOAD mysql;
INSTALL httpfs;
LOAD httpfs;
INSTALL aws;
LOAD aws;

CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    MODEL 'gpt-5.6-luna'
);

CREATE OR REPLACE SECRET s3_prod (
    TYPE s3,
    PROVIDER credential_chain,
    REGION 'us-east-1'
);

SET VARIABLE run_id = 'source-enrichment-2026-07-07T100000Z';

-- Start the job with an empty usage buffer, so the usage snapshot covers only this run.
SELECT * FROM ai_clear_usage();
```

## Attach source databases read-only

Store source credentials in DuckDB secrets. Avoid putting passwords in
connection strings, because failed connection errors can print the full string.

```sql
CREATE OR REPLACE SECRET app_postgres (
    TYPE postgres,
    HOST 'postgres.internal.example',
    PORT 5432,
    DATABASE 'app',
    USER 'readonly_ai_worker',
    PASSWORD '<password>'
);

ATTACH '' AS app_pg (
    TYPE postgres,
    SECRET app_postgres,
    READ_ONLY,
    SCHEMA 'public'
);
```

For MySQL, use the same pattern:

```sql
CREATE OR REPLACE SECRET app_mysql (
    TYPE mysql,
    HOST 'mysql.internal.example',
    PORT 3306,
    DATABASE 'app',
    USER 'readonly_ai_worker',
    PASSWORD '<password>'
);

ATTACH '' AS app_mysql (
    TYPE mysql,
    SECRET app_mysql,
    READ_ONLY
);
```

## Materialize a local working set

Filter the source table before any provider calls. This keeps scans predictable
and avoids re-reading the OLTP database during retry or export steps.

```sql
CREATE OR REPLACE TEMP TABLE source_ticket_batch AS
SELECT
    ticket_id,
    customer_id,
    priority,
    status,
    subject,
    body,
    updated_at,
    getvariable('run_id') AS run_id
FROM app_pg.public.support_tickets
WHERE status = 'open'
  AND priority IN ('high', 'urgent')
ORDER BY updated_at
LIMIT 1000;
```

If the source schema changed while the same DuckDB connection is running, clear
the extension schema cache before re-querying:

```sql
SELECT pg_clear_cache();
```

Use `mysql_clear_cache()` for attached MySQL sources.

## Enrich the local batch

Run provider calls only after the batch is local. Use `ai_try_complete` so bad
rows become rejected rows instead of aborting the whole job. `result` is a
`STRUCT` with `response` and `error`.

```sql
CREATE OR REPLACE TEMP TABLE source_ticket_attempts AS
SELECT
    run_id,
    ticket_id,
    customer_id,
    priority,
    ai_try_complete(
        'Return JSON with keys summary, next_action, risk_level. '
        || 'risk_level must be low, medium, or high.'
        || chr(10) || 'Subject: ' || subject
        || chr(10) || 'Body: ' || body,
        secret := 'openai_ai',
        response_format := 'json_object',
        retry_count := 2,
        retry_backoff_ms := 1000,
        max_concurrent_requests := 4,
        token_limit_per_minute := 200000
    ) AS result
FROM source_ticket_batch;
```

`risk_level` is a fixed scale. With a decision provider, you can skip the JSON
parsing for that field:
`ai_decide(body, {risk_level: ['low', 'medium', 'high']}, provider := 'ollama', on_error := 'null')`
returns `risk_level DOUBLE` (0 to 2) and `risk_level_confidence`. `ai_decide`
has no capture mode, so treat a NULL result as rejected. See
[route rows by decision confidence](decision-routing.md). Keep
`ai_try_complete` for free-text fields such as `summary` and `next_action`.

## Export or stage the output

For the lowest-risk production path, export success and rejection files and let a
separate application-owned process load them into the source system.

`response_format := 'json_object'` asks for JSON but does not check the reply.
A reply that is not valid JSON has `error = NULL`, and casting it to `JSON`
fails the whole statement. The `json_valid` filters below route such rows to
the rejected file. `PARTITION_BY (run_id)` writes each run to its own
`run_id=<value>/` folder.

```sql
COPY (
    SELECT
        run_id,
        ticket_id,
        customer_id,
        priority,
        result.response::JSON AS response_json,
        now() AS loaded_at
    FROM source_ticket_attempts
    WHERE result.error IS NULL
      AND json_valid(result.response)
)
TO 's3://support-prod/ai/source_ticket_enriched'
(FORMAT parquet, COMPRESSION zstd, PARTITION_BY (run_id), OVERWRITE_OR_IGNORE true);

COPY (
    SELECT
        run_id,
        ticket_id,
        customer_id,
        priority,
        coalesce(result.error, 'missing or invalid JSON response') AS error,
        now() AS loaded_at
    FROM source_ticket_attempts
    WHERE result.error IS NOT NULL
       OR NOT coalesce(json_valid(result.response), false)
)
TO 's3://support-prod/ai/source_ticket_rejected'
(FORMAT parquet, COMPRESSION zstd, PARTITION_BY (run_id), OVERWRITE_OR_IGNORE true);
```

If you intentionally write back to Postgres or MySQL, use a separate writable
attachment and write only to a staging table. Review that staging table before
merging into production tables.

```sql
CREATE OR REPLACE SECRET app_postgres_writer (
    TYPE postgres,
    HOST 'postgres.internal.example',
    PORT 5432,
    DATABASE 'app',
    USER 'ai_staging_writer',
    PASSWORD '<password>'
);

ATTACH '' AS app_pg_write (
    TYPE postgres,
    SECRET app_postgres_writer,
    SCHEMA 'public'
);

CREATE TABLE IF NOT EXISTS app_pg_write.public.ai_ticket_triage_staging (
    run_id VARCHAR,
    ticket_id BIGINT,
    customer_id VARCHAR,
    response_json JSON,
    loaded_at TIMESTAMP
);

INSERT INTO app_pg_write.public.ai_ticket_triage_staging
SELECT
    run_id,
    ticket_id,
    customer_id,
    result.response::JSON AS response_json,
    now() AS loaded_at
FROM source_ticket_attempts
WHERE result.error IS NULL
  AND json_valid(result.response);
```

Do not write provider output directly into customer-facing source columns until
you have a review, backfill, and rollback plan.

## Capture usage

Persist usage events with the same run id. `ai_usage()` keeps only the latest
1,024 events, so check `dropped_events` first. If it is above 0, the snapshot is
incomplete: use smaller batches and snapshot after each one. `ai_usage_totals()`
keeps its counters even when events are dropped.

```sql
SELECT max(dropped_events) AS dropped_events
FROM ai_usage_summary();

COPY (
    SELECT getvariable('run_id') AS run_id, now() AS captured_at, *
    FROM ai_usage()
)
TO 's3://support-prod/ai/source_ticket_usage'
(FORMAT parquet, COMPRESSION zstd, PARTITION_BY (run_id), OVERWRITE_OR_IGNORE true);

SELECT getvariable('run_id') AS run_id, * FROM ai_usage_totals();
```

## Related cookbooks

- [Run production batch enrichment from S3 or Parquet](production-batch-enrichment.md)
- [Write audited AI outputs to lakehouse tables](audited-lakehouse-output.md)
- [Monitor AI usage, failures, and cost](usage-cost-observability.md)
- [Resume a local enrichment job](resumable-enrichment.md)

## Learn more

- [DuckDB PostgreSQL extension](https://duckdb.org/docs/current/core_extensions/postgres/overview)
- [PostgreSQL secrets](https://duckdb.org/docs/current/core_extensions/postgres/secrets)
- [DuckDB MySQL extension](https://duckdb.org/docs/current/core_extensions/mysql)
