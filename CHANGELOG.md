# Changelog

All notable changes to `duckdb_ai` are documented here.

This project uses semantic versioning. Before `1.0.0`, minor versions may
include SQL API changes and patch versions should preserve the SQL API.

## Unreleased

## 0.4.13 - 2026-07-17

### Fixed

- Replaced Gemini's retired `gemini-embedding-001` default with the supported
  `gemini-embedding-2` model and refreshed its built-in text-input pricing.

## 0.4.12 - 2026-07-17

### Fixed

- Applied schema include/exclude filters before collecting table metadata and
  sample rows, avoiding unnecessary scans of tables omitted from AI prompts.

## 0.4.11 - 2026-07-15

### Fixed

- Corrected Amazon Bedrock's GPT OSS 120B default model id and replaced
  DeepInfra's retiring Llama 3.1 8B Instruct default with its Turbo successor.

### Changed

- Added built-in cost metadata for OpenAI GPT-5.6 Sol, Terra, and Luna and
  Anthropic Claude Sonnet 5.

## 0.4.10 - 2026-07-14

### Fixed

- Qualified provider executor queue accesses so MSVC resolves the member
  instead of DuckDB's `queue` template inside the worker lambda.

## 0.4.9 - 2026-07-14

### Fixed

- Used brace initialization for the provider executor's worker lock so MSVC
  does not parse the declaration as a function and fail Windows builds.
- Hardened AI SQL execution with bounded caches and concurrency, encoded
  request-limit enforcement, aggregate convergence checks, accurate recursive
  embedding-split accounting, and stricter model, classification, and result
  validation.
- Replaced Databricks' retired Llama 4 Maverick default with its documented
  OpenAI GPT OSS 120B replacement endpoint.
- Emitted llama.cpp's direct JSON Schema response-format shape instead of the
  nested OpenAI envelope.

## 0.4.8 - 2026-07-14

### Fixed

- Updated Amazon Bedrock, Cohere, and MiniMax defaults to current models from
  their public OpenAI-compatible APIs.
- Sent MiniMax and Moonshot/Kimi completion limits through the preferred
  `max_completion_tokens` field while preserving the SQL `max_tokens` option.
- Emitted Cohere's documented `json_object` plus `schema` request shape for
  structured completion output.

### Changed

- Updated the Docusaurus documentation packages from 3.10.1 to 3.10.2.

## 0.4.7 - 2026-07-10

### Fixed

- Updated Cloudflare Workers AI, Tencent TokenHub / Hunyuan, and Baidu Qianfan
  defaults to current supported endpoints and models while retaining legacy
  Hunyuan environment-variable aliases for existing configurations.
- Sent completion limits through `max_completion_tokens` for OpenAI,
  Cloudflare Workers AI, and Snowflake Cortex requests, matching their current
  public APIs while preserving the SQL `max_tokens` option.
- Removed retired Claude 3.5 Haiku built-in pricing metadata.

## 0.4.6 - 2026-07-09

### Fixed

- Updated the xAI / SpaceXAI provider default to `grok-4.5`, added built-in
  `grok-4.5` pricing metadata, and sent `prompt_cache := true` hints through
  the documented `x-grok-conv-id` chat-completions header.

## 0.4.5 - 2026-07-08

### Added

- Added first-class provider defaults, aliases, and environment-variable
  handling for Amazon Bedrock, Google Vertex AI, Groq, Together AI, Fireworks
  AI, DeepInfra, Cerebras, Cohere, GitHub Models, Hugging Face, NVIDIA NIM,
  Perplexity, xAI, Cloudflare Workers AI, Alibaba Cloud Model Studio /
  DashScope, Nebius Token Factory, SambaNova Cloud, SiliconFlow, Vercel AI
  Gateway, Moonshot AI / Kimi, Baidu Qianfan, Tencent Hunyuan, StepFun,
  MiniMax, Poe, and Volcengine Ark.
- Added provider request-shape coverage and smoke metadata checks for the
  expanded OpenAI-compatible provider catalog.
- Added cookbook docs for production batch enrichment, messy document intake,
  source database enrichment, audited lakehouse outputs, and AI usage-cost
  observability.

### Changed

- Centralized provider defaults in one provider catalog so aliases, base URLs,
  default models, embedding defaults, and required credentials stay aligned
  across SQL helpers, request builders, docs, and tests.

## 0.4.3 - 2026-07-08

### Fixed

- Updated Gemini completion defaults and built-in pricing metadata to use the
  current `gemini-3.5-flash` model from Google's OpenAI-compatible API docs.

## 0.4.2 - 2026-07-07

### Fixed

- Updated provider defaults and built-in pricing metadata for current public
  provider docs: Gemini text embeddings now default to `gemini-embedding-001`,
  DeepSeek completion calls default to `deepseek-v4-flash`, and Z.ai calls use
  `https://api.z.ai/api/paas/v4` with `glm-4.7-flash`.

## 0.4.1 - 2026-07-07

### Fixed

- Rejected non-finite `temperature` and `log_sample_rate` values from per-call
  options, DuckDB settings, and `DUCKDB_AI_LOG_SAMPLE_RATE` instead of emitting
  invalid provider request JSON or silently disabling usage-log sampling.

### Changed

- Refreshed Docusaurus transitive lockfile dependencies within existing package
  ranges.

## 0.4.0 - 2026-07-06

### Added

- Exposed function descriptions and examples through `duckdb_functions()`
  catalog metadata for every `ai_*` function.
- Added a llama.cpp server provider (`llamacpp` / `llama.cpp`) for
  OpenAI-compatible chat and embeddings against `llama-server`.
- Added bind-verified SQL self-correction (`fix_attempts := N`) across the
  SQL assistant functions (`ai_sql`, `ai_query_data`, `ai_fix_sql`), plus
  optional catalog bind checks in `ai_is_read_only_sql` and
  `ai_validate_read_only_sql`.
- Added `scripts/preview_community_docs.sh` to preview the generated
  duckdb.org community extension page locally, with a committed snapshot
  (`test/community_docs_snapshot/`) and `--check`/`--update` modes.
- Added `RELEASING.md` documenting the release and community-extensions
  publication flow.

### Changed

- Tightened the first line of the SQL assistant function descriptions and
  added second usage examples for `ai_complete`, `ai_classify`, `ai_sql`,
  `ai_query_data`, and `ai_fix_sql`.
- Documentation now leads with `INSTALL ai FROM community` now that the
  extension is published as a DuckDB community extension.

## 0.3.2 - 2026-07-03

### Fixed

- Allowed embedding functions to use the documented `cache_ttl_seconds`,
  `cache_max_entries`, and `connect_timeout_seconds` per-call options.

## 0.3.1 - 2026-07-02

### Fixed

- Defined `NOMINMAX` for Windows builds so the `windows_amd64` MSVC build
  compiles: `windows.h` (included via curl) defines `min`/`max` macros that
  broke `std::min`/`std::max` and `std::numeric_limits<T>::max()`.

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
