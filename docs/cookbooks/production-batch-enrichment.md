---
sidebar_position: 5
title: "Run production batch enrichment from S3 or Parquet"
sidebar_label: "Batch enrichment from S3 or Parquet"
description: "Run LLM enrichment over Parquet or S3 data in DuckDB with bounded batches, row-level failure capture and durable output files."
keywords: ["DuckDB LLM batch", "S3 Parquet enrichment", "ai_try_complete"]
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

- Set `OPENAI_API_KEY` in the environment before starting DuckDB. The
  `openai_ai` secret below stores the provider and model, not the key. See the
  [provider guides](../provider-guides.md).
- Configure object-storage credentials with DuckDB secrets.
- Use a stable `run_id` for every production job attempt.
- Check the prompt and model on a labeled sample first, as in
  [evaluate before a batch run](evaluate-before-batch.md).

```sql
INSTALL ai FROM community;
LOAD ai;
INSTALL json;
LOAD json;

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
    MODEL 'gpt-5.6-luna'
);

SET VARIABLE run_id = 'support-triage-2026-07-07T100000Z';

-- Start the job with an empty usage buffer, so the usage snapshot covers only this run.
SELECT * FROM ai_clear_usage();
```

For S3-compatible storage such as R2, GCS interoperability, MinIO, or lakeFS,
use the matching DuckDB secret type or endpoint options for your environment.

## Read a bounded input slice

Read Parquet files by glob, keep the input filename for lineage, and materialize
the job input locally before calling the provider. This prevents repeated scans
of the remote object store if you rerun downstream steps. The `LIMIT` caps how
many rows, and so how many provider calls, one run can make.

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
  AND priority IN ('high', 'urgent')
ORDER BY ticket_id
LIMIT 5000;
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

Use the result to adjust the `LIMIT` for a first production run, or to split
the input into multiple scheduled chunks.

## Enrich rows with failure capture

Use `ai_try_complete` so one bad row does not fail the whole query. Ask for JSON
so the successful rows can be parsed and validated downstream. `result` is a
`STRUCT` with `response` and `error`.

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

`urgency_score` is a fixed scale. With a decision provider, you can skip the
JSON parsing for that field:
`ai_decide(body, {urgency_score: ['Routine', 'Minor', 'Degraded', 'Severe', 'Production down']}, provider := 'ollama', on_error := 'null')`
returns `urgency_score DOUBLE` (a level from 0 to 4, so add 1 for the 1 to 5
scale) and `urgency_score_confidence`.
`ai_decide` has no capture mode, so treat a NULL result as rejected. See
[route rows by decision confidence](decision-routing.md). Keep `ai_try_complete`
for free-text fields such as `summary`.

## Split successes and rejected rows

Keep successful rows and rejected rows as separate datasets. Rejected rows should
retain the provider error and enough lineage to replay only those rows.

`response_format := 'json_object'` asks for JSON but does not check the reply.
A reply that is not valid JSON has `error = NULL`, and casting it to `JSON`
fails the whole statement. Filter with `json_valid` so such rows land in the
rejected table instead.

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
WHERE result.error IS NULL
  AND json_valid(result.response);

CREATE OR REPLACE TEMP TABLE ai_rejected_rows AS
SELECT
    run_id,
    ticket_id,
    customer_id,
    priority,
    source_file,
    coalesce(result.error, 'missing or invalid JSON response') AS error,
    now() AS loaded_at
FROM ai_attempts
WHERE result.error IS NOT NULL
   OR NOT coalesce(json_valid(result.response), false);
```

## Persist outputs

Write successful rows, rejected rows, and usage events to separate locations.
`PARTITION_BY (run_id)` writes each run to its own `run_id=<value>/` folder, so
a retry of one run does not touch the files of other runs. Read the folders
back with `read_parquet('.../run_id=*/*.parquet', hive_partitioning = true)`.

```sql
COPY ai_successes
TO 's3://support-prod/ai/ticket_triage'
(
    FORMAT parquet,
    COMPRESSION zstd,
    ROW_GROUP_SIZE 100000,
    PARTITION_BY (run_id),
    OVERWRITE_OR_IGNORE true
);

COPY ai_rejected_rows
TO 's3://support-prod/ai/ticket_triage_rejected'
(
    FORMAT parquet,
    COMPRESSION zstd,
    ROW_GROUP_SIZE 100000,
    PARTITION_BY (run_id),
    OVERWRITE_OR_IGNORE true
);
```

`ai_usage()` keeps only the latest 1,024 events. Check that none were dropped
before you save them. If `dropped_events` is above 0, the usage file is
incomplete: run smaller batches and snapshot after each one. `ai_usage_totals()`
keeps its counters even when events are dropped, so save it with the run:

```sql
SELECT max(dropped_events) AS dropped_events
FROM ai_usage_summary();

COPY (
    SELECT getvariable('run_id') AS run_id, now() AS captured_at, *
    FROM ai_usage()
)
TO 's3://support-prod/ai/usage'
(
    FORMAT parquet,
    COMPRESSION zstd,
    PARTITION_BY (run_id),
    OVERWRITE_OR_IGNORE true
);

COPY (
    SELECT getvariable('run_id') AS run_id, now() AS captured_at, *
    FROM ai_usage_totals()
)
TO 's3://support-prod/ai/usage_totals'
(
    FORMAT parquet,
    PARTITION_BY (run_id),
    OVERWRITE_OR_IGNORE true
);
```

After the run is durably captured, clear the in-process usage buffer if the same
DuckDB process will run another job:

```sql
SELECT * FROM ai_clear_usage();
```

## Resume after a process restart

The temporary tables above live only for the current connection. For a local,
single-writer job that skips previously successful rows after reopening DuckDB,
use the [resumable enrichment example](resumable-enrichment.md).

## Related cookbooks

- [Enrich rows from Postgres or MySQL](source-database-enrichment.md)
- [Write audited AI outputs to lakehouse tables](audited-lakehouse-output.md)
- [Monitor AI usage, failures, and cost](usage-cost-observability.md)
- [Normalize messy documents](messy-document-intake.md)

## Learn more

- [DuckDB S3 API support](https://duckdb.org/docs/current/core_extensions/httpfs/s3api)
- [DuckDB AWS extension](https://duckdb.org/docs/current/core_extensions/aws)
- [DuckDB Parquet tips](https://duckdb.org/docs/current/data/parquet/tips)
- [Reading multiple files](https://duckdb.org/docs/current/data/multiple_files/overview)
