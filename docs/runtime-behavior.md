---
sidebar_position: 5
---

# Runtime behavior

This page documents how `duckdb_ai` executes provider calls at runtime. It is
the operational reference for the provider hardening, caching, concurrency, and
egress controls used by the SQL functions.

## Function stability

Model-backed scalar and aggregate functions are registered as `VOLATILE` in
DuckDB. The planner must not constant-fold or reuse their results as if provider
calls were deterministic.

Local helper functions such as `ai_count_tokens()` and
`ai_is_read_only_sql()` remain deterministic and do not call providers.

## Runtime state scope

Usage events, response-cache entries, and provider pacing state are scoped to
the current DuckDB database instance. Separate embedded DuckDB databases in the
same process do not share these buffers.

The affected state includes:

- `ai_usage()` and `ai_clear_usage()`,
- opt-in response cache entries used by `cache := true` or `duckdb_ai_cache`,
- `duckdb_ai_max_concurrent_requests`,
- `duckdb_ai_min_request_interval_ms`, and
- `duckdb_ai_token_limit_per_minute`.

## Intra-chunk provider concurrency

Row-wise provider scalar functions prepare row inputs, options, and secrets on
the DuckDB execution thread, then run provider work in a bounded worker pool for
the current vector chunk. The DuckDB result vectors are filled after worker
completion on the execution thread.

This applies to:

- `ai_complete()`, `ai_try_complete()`, `ai_complete_json()`, and
  `ai_request_json()`,
- task wrappers such as `ai_summarize()`, `ai_classify()`, `ai_redact()`,
  `ai_translate()`, and `ai_filter()`,
- `ai_embed()`, `ai_embedding_request_json()`, and `ai_similarity()`, and
- `ai_sql()`.

The worker count is bounded by `duckdb_ai_max_concurrent_requests` or
`max_concurrent_requests := ...` when configured. Without an explicit cap, the
extension uses a conservative local worker count for the chunk while the
provider rate limiter still controls outbound request pacing.

Aggregate functions and SQL-assistant table functions perform one provider call
per group or invocation and do not need intra-chunk fan-out.

## Cancellation and retries

Provider HTTP calls use libcurl with interrupt-aware progress callbacks. When a
DuckDB query is interrupted, in-flight provider calls abort and retry sleeps are
also interruptible.

Retries are disabled by default. When enabled with `retry_count` or
`duckdb_ai_retry_count`, retryable HTTP failures use exponential backoff with
jitter. HTTP `Retry-After` headers on provider responses take precedence over
the configured backoff.

## HTTP connection behavior

The extension initializes libcurl once per process and reuses a thread-local
easy handle for provider requests. This allows libcurl to reuse connections
where the provider and libcurl build support it.

Provider redirects are not followed. This avoids forwarding authorization or
API-key headers to an unexpected redirect target.

`CURLOPT_NOSIGNAL` is enabled so DuckDB host processes are not exposed to
libcurl signal behavior during DNS or timeout handling.

## Response caching

Response caching is opt-in:

```sql
SET duckdb_ai_cache = true;

SELECT ai_complete('Summarize this repeated prompt.');
SELECT ai_complete('Summarize this repeated prompt.');

SELECT * FROM ai_clear_cache();
```

The cache is in-memory and scoped to the current DuckDB database instance. It is
keyed by provider, model, endpoint, request payload, and response-relevant
options. API keys are not stored directly in cache keys.

Cached responses still record usage events, but their elapsed time is reported
as `0` because no provider HTTP request was made.

The maximum number of cached entries defaults to `1024`. Set
`DUCKDB_AI_CACHE_MAX_ENTRIES=0` to disable storage, or use a positive integer to
change the bound.

## Egress allowlisting

Use `duckdb_ai_allowed_hosts` or per-call `allowed_hosts := ...` to restrict
provider and usage-log destinations:

```sql
SET duckdb_ai_allowed_hosts = 'ai-gateway.internal,api.openai.com';

SELECT ai_complete(
    'Summarize this.',
    allowed_hosts := 'api.openai.com'
);
```

Entries can be hostnames, `host:port`, full URLs, wildcard subdomains such as
`*.example.com`, or `*`.

The allowlist check runs before the HTTP request, so disallowed hosts fail
without sending a provider request.

## JSON parsing

Provider responses and JSON Schema inputs are parsed with DuckDB's vendored
`yyjson` parser. This covers completion text extraction, token usage fields,
embedding arrays, `ai_complete_json()` validation, and
`ai_complete_record()` projection.

`ai_complete_json()` and `ai_complete_record()` intentionally implement a
documented JSON Schema subset rather than full JSON Schema draft parity.
