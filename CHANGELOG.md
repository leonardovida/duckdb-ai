# Changelog

All notable changes to `duckdb_ai` are documented here.

This project uses semantic versioning. Before `1.0.0`, minor versions may
include SQL API changes and patch versions should preserve the SQL API.

## Unreleased

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
