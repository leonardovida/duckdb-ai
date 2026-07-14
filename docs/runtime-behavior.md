---
sidebar_position: 5
---

# Runtime behavior

This page documents how the `ai` extension executes provider calls at runtime. It is
the operational reference for the provider hardening, caching, concurrency, and
egress controls used by the SQL functions.

## Function stability

Model-backed scalar and aggregate functions are registered as `VOLATILE` in
DuckDB. The planner must not constant-fold or reuse their results as if provider
calls were deterministic.

Local helper functions such as `ai_count_tokens()` and
`ai_is_read_only_sql()` remain deterministic and do not call providers.

## Prompt shape

Completion functions accept one user prompt plus an optional system prompt.
Task wrappers and SQL assistant functions build provider messages from their SQL
arguments: static instructions and schema context are placed in the system
message when the selected provider protocol supports it, while row-specific text
stays in the user message.

Multi-turn chat transcripts are deliberately outside the scalar function
surface. Store or serialize the conversation in your own table and pass the
conversation summary or transcript as the prompt when you need row-wise SQL
enrichment.

## Runtime state scope

Usage events, response-cache entries, and provider pacing state are scoped to
the current DuckDB database instance. Separate embedded DuckDB databases in the
same process do not share these buffers.

The affected state includes:

- `ai_usage()` and `ai_clear_usage()`,
- opt-in response cache entries used by `cache := true` or `duckdb_ai_cache`,
- bounded query-local embeddings used to deduplicate `ai_similarity()` inputs,
- generated SQL cache entries for successful `ai_query_data()` binds,
- `duckdb_ai_max_concurrent_requests`,
- `duckdb_ai_min_request_interval_ms`, and
- `duckdb_ai_token_limit_per_minute`.

## Intra-chunk provider concurrency

Row-wise provider scalar functions prepare row inputs, options, and secrets on
the DuckDB execution thread, then submit provider work to a bounded executor
owned by the current DuckDB database instance. Worker threads persist across
vector chunks instead of being created once per vector. The DuckDB result
vectors are filled after worker completion on the execution thread.

This applies to:

- `ai_complete()`, `ai_try_complete()`, `ai_complete_json()`, and
  `ai_completion_request_json()`,
- task wrappers such as `ai_summarize()`, `ai_classify()`, `ai_redact()`,
  `ai_translate()`, and `ai_filter()`,
- `ai_embed()`, `ai_embedding_request_json()`, and `ai_similarity()`, and
- `ai_sql()`.

The worker count is bounded by `duckdb_ai_max_concurrent_requests` or
`max_concurrent_requests := ...` when configured. Without an explicit cap, the
extension uses a conservative local worker count for the chunk while the
provider rate limiter still controls outbound request pacing.
Configured worker caps must be between 0 and 64; larger values are rejected
instead of being silently clamped.

SQL-assistant table functions perform one provider call per invocation.
Aggregate functions use one request for groups that fit `max_context_chars` and
a bounded parallel map/reduce request tree for larger groups.

## Embedding request packing

`ai_embed()`, `ai_similarity()`, classifier training, and optimized
classification pack embedding inputs by provider/model option group. A request
is closed when the configured input count, estimated token count, or request
byte limit would be exceeded. External model `options` can override these
limits; conservative built-in defaults apply otherwise.

`ai_similarity()` deduplicates both sides of every row, keeps a bounded
query-local embedding cache across DuckDB vector chunks, embeds each distinct
value once, and computes cosine similarity locally. Its request count therefore
scales with packed distinct values rather than two HTTP requests per row. The
query cache is capped at 8 MiB and the database keeps at most eight recent query
caches; `ai_clear_cache()` clears it explicitly. Query-cache hits appear in
`ai_usage_summary()` without increasing `batch_count`.

When a provider rejects a multi-input embedding request with HTTP 413 or a
recognized payload/context-size error, the extension recursively bisects that
request. Recoverable splits do not count as terminal failures in usage
summaries. Input packing measures the encoded JSON payload, so quotes, control
characters, and other escaped content are included in the byte limit. A single
input that exceeds an external model's declared context or byte limit fails
explicitly.

## Context-safe aggregate reduction

`ai_agg()` and `ai_summarize_agg()` never truncate grouped input. Groups within
`max_context_chars` use one final request. Larger groups are split on UTF-8-safe
boundaries, mapped concurrently into compact evidence, packed, and recursively
reduced until one final request fits. Every child uses the parent query's
operation tree for usage attribution. Set `overflow_policy := 'error'` to reject
an oversized group before any provider call. Hierarchical reduction also stops
immediately with an explicit error when a provider's intermediate responses do
not reduce either the chunk count or total byte size.

## Cancellation and retries

Provider HTTP calls use libcurl with interrupt-aware progress callbacks. When a
DuckDB query is interrupted, in-flight provider calls abort and retry sleeps are
also interruptible.

Retries are disabled by default. When enabled with `retry_count` or
`duckdb_ai_retry_count`, retryable HTTP failures use exponential backoff with
jitter. HTTP `Retry-After` headers on provider responses take precedence over
the configured backoff.

Usage events keep one `operation_id` for all input events produced by a packed
request. Hierarchical aggregate children additionally carry a
`parent_operation_id`; retries retain the same operation ID and increment the
event's retry count. `ai_usage_summary()` exposes both row-event calls and
distinct request batches, plus bounded-buffer drop counters.

## HTTP connection behavior

The extension initializes libcurl once per process, reuses a thread-local easy
handle for provider requests, and attaches those handles to a shared libcurl
connection cache. This allows libcurl to reuse connections across provider
worker threads where the provider and libcurl build support it.

Provider calls use the configured `timeout_seconds` for the total request. The
connect timeout defaults to the smaller of 10 seconds and the total timeout. Set
`connect_timeout_seconds := ...`, `duckdb_ai_connect_timeout_seconds`, or
`DUCKDB_AI_CONNECT_TIMEOUT_SECONDS` to override it for provider requests.

Provider redirects are not followed. This avoids forwarding authorization or
API-key headers to an unexpected redirect target.

Provider and usage-log destinations must use `http` or `https`. URLs containing
embedded credentials or raw control characters are rejected before libcurl is
called. The same control-character check applies to generated HTTP headers.

Provider and usage-log response bodies are limited to 64 MiB by default to
prevent an untrusted endpoint from growing an unbounded in-memory buffer. Set
`DUCKDB_AI_MAX_RESPONSE_BYTES` to a positive byte count, up to 1 GiB, when a
large batched embedding response requires a different bound.

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

The response cache is in-memory and scoped to the current DuckDB database
instance. It is keyed by provider, model, endpoint, request payload, and
response-relevant options. API keys are not stored directly in cache keys.

Cached responses still record usage events, but their elapsed time is reported
as `0` because no provider HTTP request was made.

The maximum number of cached entries defaults to `1024`. Set
`cache_max_entries := ...`, `duckdb_ai_cache_max_entries`, or
`DUCKDB_AI_CACHE_MAX_ENTRIES` to change the bound. Use `0` to disable
response-cache storage. Cached response bodies and keys also have a hard 64 MiB
per-database bound; an individual response larger than that is not cached.

`ai_query_data()` also keeps a small in-memory generated-SQL cache for successful
binds. `ai_clear_cache()` clears the response, generated-SQL, and similarity
query caches.

## Deterministic performance benchmark

The local benchmark uses a threaded mock embedding endpoint and reports request
count, wall time, peak DuckDB thread count, peak resident memory, and dropped
usage events for 1,000 and 10,000 rows. It also derives a provider cost estimate
from mock token usage at a fixed price:

```sh
python3 test/benchmarks/ai_runtime_benchmark.py
```

It covers unique `ai_embed()` inputs and repeated `ai_similarity()` inputs. The
latter uses 20 distinct values at both row counts, making a regression from
query-level distinct-value scaling directly visible in `http_requests`.

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
Schema `pattern` checks use RE2-compatible regular expressions so validation
has bounded-time matching behavior; constructs unsupported by RE2 are rejected.
