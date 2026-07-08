---
sidebar_position: 8
---

# Run production batch enrichment from S3 or Parquet

Use this cookbook when source rows already live in object storage and the output
should be written back as durable files. The pattern is:

1. Read a bounded Parquet slice.
2. Estimate the batch size.
3. Call the provider with row-level failure capture.
4. Split successful rows from rejected rows.
5. Persist outputs and usage data.

## Prerequisites

- Configure a completion provider with a `TYPE duckdb_ai` secret.
- Configure object-storage credentials with DuckDB secrets.
- Use a stable `run_id` for every production job attempt.

```sql
INSTALL ai FROM community;
LOAD ai;

INSTALL httpfs;
LOAD httpfs;
INSTALL aws;
LOAD aws;

CREATE OR REPLACE SECRET s3_prod (
    TYPE s3,
    PROVIDER credential_chain,
    REGION 'us-east-1'
);

CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    MODEL 'gpt-4o-mini'
);

SET VARIABLE run_id = 'support-triage-2026-07-07T100000Z';
```

For S3-compatible storage such as R2, GCS interoperability, MinIO, or lakeFS,
use the matching DuckDB secret type or endpoint options for your environment.

## Read a bounded input slice

Read Parquet files by glob, keep the input filename for lineage, and materialize
the job input locally before calling the provider. This prevents repeated scans
of the remote object store if you rerun downstream steps.

```sql
CREATE OR REPLACE TEMP TABLE job_input AS
SELECT
    ticket_id,
    customer_id,
    priority,
    status,
    subject,
    body,
    filename AS source_file,
    getvariable('run_id') AS run_id
FROM read_parquet(
    's3://support-prod/tickets/dt=2026-07-07/*.parquet',
    union_by_name = true,
    filename = true,
    hive_partitioning = true
)
WHERE status = 'open'
  AND priority IN ('high', 'urgent');
```

Check the row count before provider calls:

```sql
SELECT count(*) AS input_rows
FROM job_input;
```

## Estimate a starting batch size

Use the local token estimator before a large run. Replace the limit values with
the provider limits you operate under.

```sql
WITH prompt_stats AS (
    SELECT
        avg(ai_count_tokens(subject || chr(10) || body)) AS input_tokens_per_row
    FROM job_input
)
SELECT ai_recommended_batch_size(
    input_tokens_per_row,
    250,
    200000,
    500
) AS recommended_rows_per_batch
FROM prompt_stats;
```

Use the result to choose the `LIMIT` for a first production run, or to split the
input into multiple scheduled chunks.

## Enrich rows with failure capture

Use `ai_try_complete` so one bad row does not fail the whole query. Ask for JSON
so the successful rows can be parsed and validated downstream.

```sql
CREATE OR REPLACE TEMP TABLE ai_attempts AS
SELECT
    run_id,
    ticket_id,
    customer_id,
    priority,
    source_file,
    ai_try_complete(
        'Return compact JSON with keys summary, product_area, urgency_score. '
        || 'Use urgency_score from 1 to 5.'
        || chr(10) || 'Subject: ' || subject
        || chr(10) || 'Body: ' || body,
        secret := 'openai_ai',
        response_format := 'json_object',
        max_tokens := 250,
        retry_count := 2,
        retry_backoff_ms := 1000,
        max_concurrent_requests := 4,
        min_request_interval_ms := 100,
        token_limit_per_minute := 200000
    ) AS result
FROM job_input;
```

## Split successes and rejected rows

Keep successful rows and rejected rows as separate datasets. Rejected rows should
retain the provider error and enough lineage to replay only those rows.

```sql
CREATE OR REPLACE TEMP TABLE ai_successes AS
SELECT
    run_id,
    ticket_id,
    customer_id,
    priority,
    source_file,
    result.response::JSON AS response_json,
    now() AS loaded_at
FROM ai_attempts
WHERE result.error IS NULL;

CREATE OR REPLACE TEMP TABLE ai_rejected_rows AS
SELECT
    run_id,
    ticket_id,
    customer_id,
    priority,
    source_file,
    result.error AS error,
    now() AS loaded_at
FROM ai_attempts
WHERE result.error IS NOT NULL;
```

## Persist outputs

Write successful rows, rejected rows, and usage events to separate locations.
Partition by `run_id` or include the run id in the output prefix so retries do
not overwrite unrelated runs.

```sql
COPY ai_successes
TO 's3://support-prod/ai/ticket_triage/run_id=2026-07-07T100000Z/'
(
    FORMAT parquet,
    COMPRESSION zstd,
    ROW_GROUP_SIZE 100000,
    OVERWRITE_OR_IGNORE true
);

COPY ai_rejected_rows
TO 's3://support-prod/ai/ticket_triage_rejected/run_id=2026-07-07T100000Z/'
(
    FORMAT parquet,
    COMPRESSION zstd,
    ROW_GROUP_SIZE 100000,
    OVERWRITE_OR_IGNORE true
);

COPY (
    SELECT getvariable('run_id') AS run_id, now() AS captured_at, *
    FROM ai_usage()
)
TO 's3://support-prod/ai/usage/run_id=2026-07-07T100000Z/'
(
    FORMAT parquet,
    COMPRESSION zstd,
    OVERWRITE_OR_IGNORE true
);
```

After the run is durably captured, clear the in-process usage buffer if the same
DuckDB process will run another job:

```sql
SELECT * FROM ai_clear_usage();
```

## Learn more

- [DuckDB S3 API support](https://duckdb.org/docs/current/core_extensions/httpfs/s3api)
- [DuckDB AWS extension](https://duckdb.org/docs/current/core_extensions/aws)
- [DuckDB Parquet tips](https://duckdb.org/docs/current/data/parquet/tips)
- [Reading multiple files](https://duckdb.org/docs/current/data/multiple_files/overview)
