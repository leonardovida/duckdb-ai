# Changelog

All notable changes to `duckdb_ai` are documented here.

This project uses semantic versioning. Before `1.0.0`, minor versions may
include SQL API changes and patch versions should preserve the SQL API.

## Unreleased

## 0.3.0 - 2026-07-02

### SQL API changes

- Renamed the extension from `duckdb_ai` to `ai` ahead of the community
  extension submission: use `INSTALL ai FROM community; LOAD ai;`. Function
  names, `TYPE duckdb_ai` secrets, `duckdb_ai_*` settings, and `DUCKDB_AI_*`
  environment variables are unchanged.

## 0.2.0 - 2026-07-02

### SQL API changes

- Renamed `ai_request_json` to `ai_completion_request_json` and `ai_models` to
  `ai_model_prices`.
- Merged `ai_fix_sql_line` into `ai_fix_sql` behind `mode := 'line'`; passing
  `error := ...` also selects line mode.
- Renamed the `duckdb_ai_sql_model` setting to `duckdb_ai_sql_assistant_model`.
- The canonical Anthropic provider name is now `anthropic`; `claude` remains an
  accepted alias. The Anthropic default model is now `claude-haiku-4-5`.
- `response_schema` now raises an explicit error on the Anthropic protocol
  instead of being silently ignored.

### Added

- `ai_extract_record(text, response_schema)` scalar function returning typed
  per-row `STRUCT` values.
- `ai_rerank(query, candidate)` and `ai_classify_labels(text, labels)`
  (multi-label classification returning `VARCHAR[]`); `ai_classify` also
  accepts a `VARCHAR[]` label list.
- `on_error := 'fail' | 'null' | 'capture'` across function families,
  subsuming `fail_on_error` (kept as a compatibility alias).
- Provider-side prompt caching hints behind `prompt_cache := true` /
  `duckdb_ai_prompt_cache`: `prompt_cache_key` for OpenAI and
  `cache_control` breakpoints for Anthropic, with cached token counts parsed
  into usage and cost estimation.
- Batched embedding requests: `ai_embed` sends chunk inputs in batched provider
  requests (up to 512 inputs per request), including when the response cache is
  enabled.
- Response-cache TTL (`cache_ttl_seconds` / `duckdb_ai_cache_ttl_seconds`),
  LRU eviction, and in-flight coalescing of identical requests.
- Failed provider calls are recorded in `ai_usage()` with new `function_name`,
  `query_id`, `cached_prompt_tokens`, `cache_creation_prompt_tokens`,
  `retries`, `cache_hit`, `status`, and `error` columns.
- Task-wrapper, JSON, and SQL-assistant instructions moved into provider
  system prompts to form stable cacheable prefixes.
- Connect timeouts (`DUCKDB_AI_CONNECT_TIMEOUT_SECONDS`), a `User-Agent`
  header, and truncation detection that raises when responses stop at
  `max_tokens`.

### Fixed and performance

- Structural response parsing per provider protocol (single JSON parse per
  response) instead of recursive first-match field scans.
- Shared libcurl connection, DNS, and TLS-session caches across worker threads
  with per-lock-class mutexes.
- Response-cache LRU updates are O(1); entries whose responses fail parsing
  (for example truncated output) are evicted instead of poisoning later hits.
- Coalesced duplicate requests are interruptible and no longer double-count
  tokens and cost in usage events.
- Queued best-effort usage logs are posted from a background worker and dropped
  on shutdown instead of blocking database close.
- Retry backoff no longer holds a concurrency slot while sleeping; secrets and
  environment configuration are resolved once per chunk/bind instead of per
  row; query interrupts are no longer recorded as provider errors.

### Runtime hardening (shipped after the 0.1.0 tag)

- Added per-database runtime state for usage events, response caching, and rate
  limiting.
- Added opt-in response caching through `cache := true`, `duckdb_ai_cache`, and
  `ai_clear_cache()`.
- Added provider/log egress allowlisting through `allowed_hosts := ...` and
  `duckdb_ai_allowed_hosts`.
- Added bounded intra-chunk provider fan-out for row-wise scalar provider
  functions while preserving DuckDB-thread result vector writes.
- Hardened provider HTTP behavior with one-time libcurl initialization,
  per-thread easy-handle reuse, `CURLOPT_NOSIGNAL`, redirect disabling,
  cancellation checks, and `Retry-After` aware exponential backoff with jitter.
- Replaced provider response and JSON Schema parsing paths with DuckDB's
  vendored `yyjson` parser.
- Documented provider-backed scalar and aggregate functions as volatile and
  verified their catalog stability metadata.
- Added security/data-flow documentation and a root security policy.
- Updated CI so pull requests run release build, SQLLogic tests, and the
  deterministic mock-provider smoke test.

## 0.1.0 - 2026-07-01

- Initial release: completion, task, embedding, aggregate, and SQL-assistant
  functions across Ollama, OpenAI, Azure, Anthropic, Gemini, Mistral,
  DeepSeek, OpenRouter, Databricks, Snowflake, Z.ai, Privacy Filter, and
  OpenAI-compatible providers, with DuckDB secrets, request-preview functions,
  usage tracking, and batch rate controls.
