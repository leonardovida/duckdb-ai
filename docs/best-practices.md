---
sidebar_position: 4
---

# Best practices

Use this page as the operating guide for running duckdb-ai in notebooks,
batch jobs, local workflows, and managed environments.

## Choose the right provider path

Use the most constrained provider that fits the job:

| Workload | Recommended path | Why |
| --- | --- | --- |
| Local development without hosted credentials | `ollama` | Keeps prompts local and avoids provider costs. |
| Production chat, extraction, classification, and SQL assistant calls | A managed LLM provider such as `openai`, `azure`, `databricks`, `snowflake`, `anthropic`, `gemini`, `mistral`, `deepseek`, `zai`, or `openrouter` | Gives managed reliability, model access, and governance controls. |
| OpenAI-compatible gateways, vLLM, LM Studio, LiteLLM, or local Ollama `/v1` | `openai_compatible` or `local` | Uses the common chat-completions request shape. |
| PII redaction where raw text should stay local | `openai_privacy_filter` with local hosting | Sends raw text to a dedicated redaction service instead of a general chat model. |
| PII redaction shared across teams | `openai_privacy_filter` with a private cloud service | Centralizes deployment, auth, scaling, and audit controls while preserving the dedicated redaction contract. |

`ai_redact` behaves differently for `openai_privacy_filter`: it sends the raw
input text to `POST /redact`. With other completion providers, it uses the
general task prompt for masking.

## Configure credentials safely

Prefer environment variables or DuckDB secrets. Avoid embedding API keys in SQL
files, notebooks, CI logs, or shell history.

```sql
CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    MODEL 'gpt-4o-mini'
);

SELECT ai_complete('hello', secret := 'openai_ai');
```

When a secret includes `API_KEY`, `ai_secrets()` reports only whether a key is
present. It does not print the key value.

Use provider-specific environment variables for local development:

| Provider | Credential env var | Base URL env var |
| --- | --- | --- |
| `openai` | `OPENAI_API_KEY` | `OPENAI_BASE_URL` |
| `azure` | `AZURE_OPENAI_API_KEY` | `AZURE_OPENAI_BASE_URL` or `AZURE_OPENAI_ENDPOINT` |
| `anthropic` / `claude` | `ANTHROPIC_API_KEY` or `CLAUDE_API_KEY` | `ANTHROPIC_BASE_URL` or `CLAUDE_BASE_URL` |
| `gemini` | `GEMINI_API_KEY` | `GEMINI_BASE_URL` |
| `mistral` | `MISTRAL_API_KEY` | `MISTRAL_BASE_URL` |
| `zai` | `ZAI_API_KEY` | `ZAI_BASE_URL` |
| `deepseek` | `DEEPSEEK_API_KEY` | `DEEPSEEK_BASE_URL` |
| `openrouter` | `OPENROUTER_API_KEY` | `OPENROUTER_BASE_URL` |
| `databricks` | `DATABRICKS_TOKEN` | `DATABRICKS_HOST` or `DATABRICKS_BASE_URL` |
| `snowflake` | `SNOWFLAKE_PAT` or `SNOWFLAKE_TOKEN` | `SNOWFLAKE_ACCOUNT_URL`, `SNOWFLAKE_HOST`, `SNOWFLAKE_ACCOUNT`, or `SNOWFLAKE_BASE_URL` |
| `openai_privacy_filter` | `OPENAI_PRIVACY_FILTER_API_KEY` | `OPENAI_PRIVACY_FILTER_BASE_URL` |
| `openai_compatible` / `local` | `OPENAI_COMPATIBLE_API_KEY` | `OPENAI_COMPATIBLE_BASE_URL` |

`DUCKDB_AI_API_KEY`, `DUCKDB_AI_BASE_URL`, and `DUCKDB_AI_MODEL` are generic
fallbacks. Use them only when one process talks to one provider; provider-
specific variables are clearer in mixed-provider environments.

## Set defaults deliberately

Use global defaults for short interactive sessions:

```sql
SET duckdb_ai_provider = 'openai';
SET duckdb_ai_model = 'gpt-4o-mini';
SET duckdb_ai_timeout_seconds = 120;
```

Use family-specific model settings when different function groups need different
models:

```sql
SET duckdb_ai_completion_model = 'gpt-4o-mini';
SET duckdb_ai_task_model = 'gpt-4o-mini';
SET duckdb_ai_sql_assistant_model = 'gpt-4o';
SET duckdb_ai_aggregate_model = 'gpt-4o-mini';
SET duckdb_ai_embedding_model = 'text-embedding-3-small';
```

Precedence is:

1. Per-call `model := ...`
2. Function-family setting such as `duckdb_ai_embedding_model`
3. `duckdb_ai_model`
4. Provider default model

For repeatable jobs, prefer secrets and explicit per-job settings over relying
on ambient environment variables.

## Restrict provider egress

Use an allowlist when the extension should only call approved providers or an
internal AI gateway:

```sql
SET duckdb_ai_allowed_hosts = 'ai-gateway.internal,api.openai.com';

SELECT ai_complete(
    'Summarize this.',
    provider := 'openai',
    allowed_hosts := 'api.openai.com'
);
```

Allowlist entries match the URL host. Entries may be hostnames, `host:port`,
full URLs, `*.example.com`, or `*`. The same control applies to outbound usage
log endpoints.

## Preview requests before calling providers

Use request-preview functions in tests, reviews, and provider debugging:

```sql
SELECT ai_completion_request_json(
    'Summarize this.',
    provider := 'snowflake',
    model := 'snowflake-llama-3.3-70b'
);

SELECT ai_embedding_request_json(
    'DuckDB vector search',
    provider := 'openai',
    model := 'text-embedding-3-small'
);
```

These functions do not make network calls. They are the safest way to confirm
model selection, message structure, response-format hints, and provider aliases.

## Prefer structured output for data pipelines

For pipelines, avoid parsing prose. Use `ai_complete_json` or
`ai_complete_record` with a JSON Schema:

```sql
SELECT *
FROM ai_complete_record(
    'Extract name and urgency from: customer says checkout is down',
    '{
      "type": "object",
      "properties": {
        "name": {"type": "string"},
        "urgency": {"type": "string"}
      },
      "required": ["urgency"],
      "additionalProperties": false
    }',
    provider := 'openai'
);
```

Keep schemas narrow. Add `required`, `additionalProperties: false`, bounded
strings, enum-like labels, and typed arrays when you need stable downstream
columns.

## Keep generated SQL bounded

Use `ai_schema_prompt` to inspect exactly what table context the SQL assistant
will see:

```sql
SELECT summary
FROM ai_schema_prompt(
    include_tables := ['main.support_tickets'],
    sample_rows := 3
);
```

Use `include_tables` and `exclude_tables` aggressively. Avoid sending an entire
database catalog when the question only needs one schema or table.

Generated SQL is validated as one read-only DuckDB `SELECT`, but you should
still inspect generated queries before using them in important workflows:

```sql
WITH generated AS (
    SELECT ai_sql(
        'Count open tickets by priority',
        include_tables := ['main.support_tickets']
    ) AS sql
)
SELECT ai_validate_read_only_sql(sql)
FROM generated;
```

## Treat redaction as a privacy control, not a proof

Use `openai_privacy_filter` for dedicated PII masking when raw text should not
be sent to a general chat model. Host it locally for the strongest data-boundary
story:

```sql
CREATE OR REPLACE SECRET privacy_filter_local (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai_privacy_filter',
    BASE_URL 'http://localhost:8080'
);
```

For high-sensitivity domains, evaluate redaction output on representative data,
keep human review paths, and avoid treating model redaction as a compliance
certificate.

## Control cost and throughput

Provider calls happen per row unless you aggregate or pre-filter first. Reduce
cost and latency by:

- Filtering rows before AI calls.
- Deduplicating repeated input text.
- Using `ai_agg` or `ai_summarize_agg` when one grouped call is enough.
- Persisting embeddings instead of recomputing pairwise similarity.
- Setting lower `max_tokens` values for classification and extraction tasks.

For batch workloads, set per-database concurrency and pacing:

```sql
SET duckdb_ai_max_concurrent_requests = 4;
SET duckdb_ai_min_request_interval_ms = 100;
SET duckdb_ai_token_limit_per_minute = 200000;
```

Row-wise provider scalar functions can run provider work concurrently within a
DuckDB vector chunk while still respecting the per-database request and token
pacing controls. See [Runtime behavior](runtime-behavior.md) for the exact
function coverage and worker limits.

Response caching is opt-in and in-memory for the current DuckDB database
instance. Enable it when repeated prompts or embeddings should not repay the
same provider call:

```sql
SET duckdb_ai_cache = true;
SELECT ai_complete('Summarize this repeated prompt.');
SELECT * FROM ai_clear_cache();
```

Use `ai_count_tokens` and `ai_recommended_batch_size` before large jobs. Keep
provider limits in a small table so the numbers are easy to update when your
account tier changes:

```sql
CREATE OR REPLACE TABLE ai_provider_limits AS
SELECT
    'openai' AS provider,
    'gpt-4o-mini' AS model,
    200000::BIGINT AS token_limit_per_minute,
    500::BIGINT AS request_limit_per_minute;

WITH prompt_stats AS (
    SELECT avg(ai_count_tokens(subject || ': ' || body)) AS input_tokens_per_row
    FROM support_tickets
)
SELECT ai_recommended_batch_size(
    input_tokens_per_row,
    200,
    token_limit_per_minute,
    request_limit_per_minute
) AS recommended_rows_per_minute
FROM prompt_stats, ai_provider_limits
WHERE provider = 'openai' AND model = 'gpt-4o-mini';
```

Retries are disabled by default. Enable small retry counts only for transient
provider failures:

```sql
SELECT ai_complete(
    'Summarize this.',
    retry_count := 2,
    retry_backoff_ms := 1000
);
```

Retry sleeps are interruptible and provider `Retry-After` headers take
precedence over the configured backoff. Provider redirects are not followed.

Use `on_error := 'null'` for exploratory enrichment where a missing output
is acceptable:

```sql
SELECT ai_classify(
    subject,
    'billing, support, sales, other',
    on_error := 'null'
)
FROM support_tickets;
```

For production enrichment, prefer `ai_try_complete` so failed rows keep their
error reason:

```sql
CREATE TEMP TABLE ticket_ai_attempts AS
SELECT
    ticket_id,
    ai_try_complete(
        subject || ': ' || body,
        provider := 'openai',
        model := 'gpt-4o-mini',
        max_tokens := 200,
        token_limit_per_minute := 200000
    ) AS result
FROM support_tickets;

CREATE OR REPLACE TABLE ticket_summaries AS
SELECT ticket_id, result.response AS summary
FROM ticket_ai_attempts
WHERE result.error IS NULL;

CREATE OR REPLACE TABLE ticket_ai_failed_rows AS
SELECT ticket_id, result.error AS error_reason, current_timestamp AS failed_at
FROM ticket_ai_attempts
WHERE result.error IS NOT NULL;
```

Use the same rejected-row `SELECT` inside `COPY (...) TO 'failed_rows.parquet'`
or an `s3://...` target when failures should live outside the DuckDB database.

## Export provider-native batch requests for offline jobs

`duckdb_ai` runs provider calls synchronously inside the DuckDB query. For
overnight jobs that should use provider-native Batch APIs, materialize request
bodies with the request-preview functions, then submit and poll them with your
job runner or provider SDK:

```sql
COPY (
    SELECT
        ticket_id AS custom_id,
        ai_completion_request_json(
            subject || chr(10) || body,
            provider := 'openai',
            model := 'gpt-4o-mini',
            system_prompt := 'Summarize this support ticket in one sentence.'
        ) AS body
    FROM support_tickets
) TO 'openai-batch-requests.jsonl' (FORMAT json);
```

Store `custom_id` with the provider response and join it back to the source
table after the batch finishes. Keep `response_schema` or downstream DuckDB
validation in the exported request path so the offline job has the same shape
checks as interactive SQL enrichment.

## Use provider-side prompt caching for stable prefixes

Keep static instructions, schemas, and catalog context byte-stable across rows.
`ai_complete_json` and `ai_complete_record` put their JSON instructions and
schema in the system message so the row-specific text stays last. Task wrappers
put their static task instructions in the system message, and SQL assistant
functions put their static rules and schema context there for the same reason.

For providers that expose cache controls, opt in with `prompt_cache := true` or
`DUCKDB_AI_PROMPT_CACHE=1`:

```sql
SELECT ai_complete_json(
    body,
    provider := 'openai',
    prompt_cache := true,
    response_schema := '{"type":"object","properties":{"summary":{"type":"string"}}}'
)
FROM documents;
```

OpenAI-compatible prompt caching generally needs a stable prefix of at least
1,024 tokens before cached-token discounts appear. Anthropic cache reads and
writes are exposed in `ai_usage()` as `cached_prompt_tokens` and
`cache_creation_prompt_tokens`.

This is separate from the extension's exact-response cache. Use `cache := true`
when identical prompts may repeat inside or across chunks in the same database
process. Concurrent identical misses are coalesced into one provider request,
and `cache_ttl_seconds := ...` or `DUCKDB_AI_CACHE_TTL_SECONDS` can expire
entries by age.

## Log usage without leaking text

`ai_usage()` keeps recent events in process:

```sql
SELECT function_name, query_id, provider, model, elapsed_ms, http_status, estimated_cost_usd
FROM ai_usage()
ORDER BY event_id DESC;
```

Outbound usage logs omit prompt, input, and response text by default:

```sql
SET duckdb_ai_log_endpoint = 'https://collector.example/ai-usage';
SET duckdb_ai_log_format = 'generic_json';
SET duckdb_ai_log_tags = 'app=support-triage,env=prod';
SET duckdb_ai_log_sample_rate = 0.25;
```

Only enable text logging when you have a documented retention and access-control
reason:

```sql
SET duckdb_ai_log_include_text = true;
```

By default, outbound usage logs are queued asynchronously and sent on a
best-effort basis. Use `duckdb_ai_log_strict = true` only when losing a usage log
must fail the SQL query; strict logs are posted synchronously.

## Corporate proxy and TLS

Provider HTTP calls use libcurl. Standard proxy and CA environment variables
such as `HTTPS_PROXY`, `HTTP_PROXY`, `NO_PROXY`, `SSL_CERT_FILE`, and
`CURL_CA_BUNDLE` are honored by libcurl in typical deployments. Prefer an
internal gateway plus `duckdb_ai_allowed_hosts` when company policy requires all
model traffic to pass through managed egress.

## Estimate cost carefully

Cost estimates are optional. Use explicit prices for a job, or opt into the
built-in catalog when it covers your model:

```sql
SET duckdb_ai_use_builtin_model_prices = true;
SELECT * FROM ai_model_prices();
```

Treat `ai_model_prices()` as a convenience catalog, not a billing source of truth.
Provider pricing and regional availability change; verify prices with the
provider before using estimates for chargeback or budgeting.
