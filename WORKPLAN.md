# duckdb_ai Work Plan

This plan is derived from the local reference corpus in
`local-docs/ai-sql-reference.md` and from the initial DuckDB extension scaffold.
The local corpus is intentionally ignored by Git.

## Phase 0 - Reference and Scaffold

- [x] Scrape the referenced MotherDuck, Databricks, Snowflake, DuckDB, and
  extension-template docs into a local markdown corpus.
- [x] Scaffold from `duckdb/extension-template`.
- [x] Rename the template extension to `duckdb_ai`.
- [x] Replace template sample functions with deterministic AI provider metadata
  and request-shape helpers.

## Phase 1 - Provider Runtime MVP

- [x] Add a `libcurl` HTTP execution path.
- [x] Support local Ollama chat requests.
- [x] Support OpenAI-compatible chat-completions providers: OpenAI, Gemini,
  Mistral, Zai, DeepSeek, OpenRouter, Databricks Model Serving, and Snowflake
  Cortex REST.
- [x] Support Claude through Anthropic's messages API.
- [x] Support OpenAI Privacy Filter through a dedicated redaction endpoint for
  local or cloud-hosted PII masking.
- [x] Keep API keys out of SQL by resolving them from environment variables.
- [x] Add privacy-minimized usage logging to an optional HTTP endpoint.
- [x] Add DuckDB settings for provider, global model, family-specific model
  defaults, base URL, timeout, reliability controls, cost controls, and logging
  endpoint once the target DuckDB settings API is verified against the pinned
  submodule.
- [x] Add DuckDB Secret support for provider credentials.

## Phase 2 - SQL Function Surface

- [x] Add `ai_complete(prompt[, model[, provider]])`.
- [x] Add `ai_request_json(prompt[, model[, provider]])` for deterministic
  testing and debugging.
- [x] Add provider metadata helpers.
- [x] Add named-parameter binding for `model`, `provider`, `temperature`,
  `system_prompt`, `max_tokens`, `base_url`, `timeout_seconds`, and
  `fail_on_error`.
- [x] Add `ai_embed(text[, model[, provider]])` returning a DuckDB array/vector.
- [x] Add task-specific convenience functions inspired by the reference corpus:
  `ai_summarize`, `ai_classify`, `ai_extract`, `ai_translate`,
  `ai_fix_grammar`, `ai_sentiment`, `ai_redact`, and `ai_similarity`.
- [x] Add `ai_sql` with markdown cleanup and deterministic single-`SELECT`
  validation.
- [x] Add aggregate forms `ai_agg` and `ai_summarize_agg`.
- [x] Add `ai_query_data` as a bind-time, read-only generated SQL execution
  path.
- [x] Add `ai_schema_prompt` as deterministic local DuckDB catalog context.
- [x] Add the remaining schema-aware SQL assistant functions: `ai_explain_sql`,
  `ai_fix_sql`, and `ai_fix_sql_line`.

## Phase 3 - Safety and Semantics

- [x] Add full output-schema enforcement for JSON/STRUCT responses.
  - [x] Add `response_format`/`response_schema` request controls,
    `ai_complete_json`, provider-native OpenAI/Ollama structured-output hints,
    and local JSON syntax validation.
  - [x] Add local `ai_complete_json` enforcement for common JSON Schema
    keywords: `type`, `properties`, `required`, `additionalProperties`,
    `items`, `enum`, and `const`.
  - [x] Expand local JSON Schema enforcement to string, numeric, array, object,
    and composition constraints: length/pattern, min/max/multipleOf,
    min/max/unique/contains items, min/max properties, pattern properties,
    property names, dependent required fields, and allOf/anyOf/oneOf/not.
  - [x] Add `ai_complete_record` for single-row typed projection from top-level
    JSON object schemas into DuckDB columns.
  - [x] Add deeper nested STRUCT/LIST projection for nested object schemas.
  - [x] Decide whether to pursue full JSON Schema draft parity beyond the
    pragmatic local subset. Decision: keep the initial OSS contract to the
    documented local subset plus provider-native hints, and revisit full draft
    compatibility after user demand is clear.
- [x] Add deterministic read-only SQL validation helpers.
- [x] Wire read-only SQL validation into generated SQL execution paths before
  `ai_query_data` can run model-produced SQL.
- [x] Add prompt/schema context extraction using DuckDB catalog metadata, with
  explicit include controls.
- [x] Add exclude controls and bounded sample query hints for generated SQL
  context.
- [x] Add actual table-row sampling for execution-time schema context, while
  keeping bind-time SQL assistant paths on safe sample-query hints.
- [x] Add SQL-visible `fail_on_error` behavior for completion calls.
- [x] Add broader provider error normalization across protocols.
- [x] Add retry policy controls that default to no hidden retries inside SQL
  execution.
- [x] Add rate-limit and concurrency controls for provider calls.
- [x] Redact secrets from errors, logs, request previews, and tests.

## Phase 4 - Observability and Cost Control

- [x] Emit optional usage events with provider, model, token counts, character
  counts, elapsed milliseconds, and HTTP status.
- [x] Add a local DuckDB table function for recent in-process usage events,
  including local `ai_schema_prompt` calls.
- [x] Add pluggable log exporters for OpenTelemetry-compatible collectors and
  generic HTTP endpoints.
- [x] Add configurable sampling and per-query tags.
- [x] Add user-supplied token price cost-estimation hooks for usage events and
  logs.
- [x] Add built-in provider/model pricing metadata for common models, expose it
  through `ai_models()`, and make catalog-backed cost estimation opt-in.

## Phase 5 - Quality Gates

- [x] Add SQLLogicTests for deterministic provider metadata and request builders.
- [x] Add deterministic tests for JSON escaping/parsing and provider
  resolution.
- [x] Add a mock HTTP server smoke test for OpenAI-compatible completion and log
  endpoint paths.
- [x] Add mock HTTP server tests for Ollama, Claude, Databricks, Snowflake, and
  OpenAI Privacy Filter paths.
- [x] Add live smoke-test documentation for local Ollama and credentialed remote
  providers.
- [x] Validate build and tests against the pinned DuckDB submodule on macOS.
- [x] Validate build and tests against the pinned DuckDB submodule on Linux.

## Phase 6 - Distribution

- [x] Remove any remaining template-specific wording.
- [x] Review the GitHub Actions distribution workflow.
- [x] Decide whether this should target DuckDB community extensions or a custom
  unsigned extension repository first.
- [x] Add pre-1.0 release and versioning documentation.
- [x] Add stable release and versioning documentation once the public API
  stabilizes.
