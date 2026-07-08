---
sidebar_position: 11
---

# Monitor AI usage, failures, and cost

Use this cookbook when a production job needs basic operational visibility:
calls, latency, failures, retries, cache hits, token counts, and estimated cost.

`ai_usage()` is in-process state for the current DuckDB database instance, so
capture it before the process exits or before you call `ai_clear_usage()`.

## Prerequisites

- Configure provider credentials.
- Set a run id before provider calls.
- Decide where usage snapshots should be persisted.

```sql
INSTALL ai FROM community;
LOAD ai;

SET VARIABLE run_id = 'usage-monitoring-2026-07-07T100000Z';
SET duckdb_ai_use_builtin_model_prices = true;
```

If the built-in price catalog is not appropriate for your provider contract,
pass explicit prices on the production calls instead:

```sql
SELECT ai_complete(
    'Summarize one production incident.',
    provider := 'openai',
    model := 'gpt-4o-mini',
    input_token_price_per_million := 0.15,
    output_token_price_per_million := 0.60
);
```

## Add job tags to outbound logs

Outbound logs are optional. They omit prompt, input, and response text by
default.

```sql
SET duckdb_ai_log_endpoint = 'https://collector.example/ai-usage';
SET duckdb_ai_log_format = 'otlp_json';
SET duckdb_ai_log_tags = 'service=support-pipeline,env=prod';
SET duckdb_ai_log_strict = false;
```

Use strict logging only when the SQL query should fail if the collector is
unavailable.

## Snapshot local usage events

Capture events after each production run:

```sql
CREATE TABLE IF NOT EXISTS ai_usage_events AS
SELECT
    getvariable('run_id') AS run_id,
    now() AS captured_at,
    *
FROM ai_usage()
LIMIT 0;

INSERT INTO ai_usage_events
SELECT
    getvariable('run_id') AS run_id,
    now() AS captured_at,
    *
FROM ai_usage();
```

Persist the snapshot before clearing the usage buffer:

```sql
COPY ai_usage_events
TO 's3://support-prod/ai/observability/usage/run_id=2026-07-07T100000Z/'
(FORMAT parquet, COMPRESSION zstd, OVERWRITE_OR_IGNORE true);

SELECT * FROM ai_clear_usage();
```

## Summarize cost and latency

Aggregate by provider, model, function, and status:

```sql
SELECT
    provider,
    model,
    function_name,
    status,
    count(*) AS calls,
    sum(prompt_tokens) AS prompt_tokens,
    sum(completion_tokens) AS completion_tokens,
    sum(total_tokens) AS total_tokens,
    sum(estimated_cost_usd) AS estimated_cost_usd,
    avg(elapsed_ms) AS avg_elapsed_ms,
    quantile_cont(elapsed_ms, 0.95) AS p95_elapsed_ms
FROM ai_usage_events
GROUP BY ALL
ORDER BY estimated_cost_usd DESC NULLS LAST;
```

Find rows that failed or retried:

```sql
SELECT
    event_id,
    function_name,
    provider,
    model,
    http_status,
    retries,
    error
FROM ai_usage_events
WHERE status <> 'ok'
   OR retries > 0
ORDER BY event_id DESC;
```

## Build alertable checks

These queries are useful as scheduler assertions after a run.

```sql
WITH totals AS (
    SELECT
        count(*) AS calls,
        count(*) FILTER (WHERE status <> 'ok') AS failed_calls,
        sum(estimated_cost_usd) AS estimated_cost_usd
    FROM ai_usage_events
)
SELECT
    calls,
    failed_calls,
    failed_calls::DOUBLE / nullif(calls, 0) AS failure_rate,
    estimated_cost_usd
FROM totals;
```

Alert when `failure_rate` or `estimated_cost_usd` crosses your job budget.

## Reconcile usage to output rows

Usage events are call-level, while output rows are job-level. Store both with the
same run id so they can be reconciled later.

```sql
SELECT
    output.run_id,
    count(*) AS output_rows,
    usage.calls,
    usage.failed_calls,
    usage.estimated_cost_usd
FROM ai_ticket_enriched AS output
JOIN (
    SELECT
        run_id,
        count(*) AS calls,
        count(*) FILTER (WHERE status <> 'ok') AS failed_calls,
        sum(estimated_cost_usd) AS estimated_cost_usd
    FROM ai_usage_events
    GROUP BY run_id
) AS usage
    ON output.run_id = usage.run_id
GROUP BY output.run_id, usage.calls, usage.failed_calls, usage.estimated_cost_usd;
```

## Learn more

- [SQL function reference: usage functions](../functions.md#usage-and-catalog-table-functions)
- [Best practices: logging and cost](../best-practices.md#log-usage-without-leaking-text)
- [Runtime behavior: usage and cache state](../runtime-behavior.md#runtime-state-scope)
