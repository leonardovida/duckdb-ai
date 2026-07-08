---
sidebar_position: 9
---

# Enrich rows from Postgres or MySQL safely

Use this cookbook when production rows live in an OLTP database. The safe
pattern is to attach the source read-only, materialize a bounded local working
set, run AI enrichment locally, and write results to an explicit staging target.

## Prerequisites

- Use read-only database credentials for the source attachment.
- Configure a completion provider with a `TYPE duckdb_ai` secret.
- Configure object-storage credentials if the enriched output will be exported to
  S3-compatible storage.
- Decide whether enriched rows should be exported to files or inserted into a
  separate staging table in the source database.

```sql
INSTALL ai FROM community;
LOAD ai;

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
    MODEL 'gpt-4o-mini'
);

CREATE OR REPLACE SECRET s3_prod (
    TYPE s3,
    PROVIDER credential_chain,
    REGION 'us-east-1'
);

SET VARIABLE run_id = 'source-enrichment-2026-07-07T100000Z';
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
rows become rejected rows instead of aborting the whole job.

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

## Export or stage the output

For the lowest-risk production path, export success and rejection files and let a
separate application-owned process load them into the source system.

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
)
TO 's3://support-prod/ai/source_ticket_enriched/run_id=2026-07-07T100000Z/'
(FORMAT parquet, COMPRESSION zstd, OVERWRITE_OR_IGNORE true);

COPY (
    SELECT
        run_id,
        ticket_id,
        customer_id,
        priority,
        result.error AS error,
        now() AS loaded_at
    FROM source_ticket_attempts
    WHERE result.error IS NOT NULL
)
TO 's3://support-prod/ai/source_ticket_rejected/run_id=2026-07-07T100000Z/'
(FORMAT parquet, COMPRESSION zstd, OVERWRITE_OR_IGNORE true);
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
    customer_id BIGINT,
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
WHERE result.error IS NULL;
```

Do not write provider output directly into customer-facing source columns until
you have a review, backfill, and rollback plan.

## Capture usage

Persist usage events with the same run id:

```sql
COPY (
    SELECT getvariable('run_id') AS run_id, now() AS captured_at, *
    FROM ai_usage()
)
TO 's3://support-prod/ai/source_ticket_usage/run_id=2026-07-07T100000Z/'
(FORMAT parquet, COMPRESSION zstd, OVERWRITE_OR_IGNORE true);
```

## Learn more

- [DuckDB PostgreSQL extension](https://duckdb.org/docs/current/core_extensions/postgres/overview)
- [PostgreSQL secrets](https://duckdb.org/docs/current/core_extensions/postgres/secrets)
- [DuckDB MySQL extension](https://duckdb.org/docs/current/core_extensions/mysql)
