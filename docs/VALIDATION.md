# Validation

This file records local validation evidence for the pinned DuckDB extension
build. It is not a substitute for CI history; update it when release gates are
run on a new platform or DuckDB submodule revision.

## 2026-07-01 0.1.0 macOS release-candidate validation

Environment:

- Platform: macOS 26.3, Darwin 25.3.0, arm64
- DuckDB submodule: `08e34c44`
- extension-ci-tools submodule: `b777c70`
- CMake: `4.3.4`
- Ninja: `1.13.2`
- Extension version: `0.1.0`

Commands:

```sh
PATH=/tmp/duckdb_ai_format_venv/bin:$PATH GEN=ninja make format-check
npm run typecheck
npm run build
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
PATH=/tmp/duckdb_ai_format_venv/bin:$PATH GEN=ninja make tidy-check
```

Result:

- Format check passed.
- Docusaurus typecheck and production build passed.
- Release build passed and linked `extension/duckdb_ai/duckdb_ai.duckdb_extension`.
- CMake loaded `duckdb_ai` from this repository at `0.1.0`.
- SQLLogic tests passed with 154 assertions.
- Mock provider smoke passed.
- Clang-tidy check passed.

Release notes:

- This is a pre-community preview source build. Public binary distribution is
  not configured for `0.1.0`.
- This validation includes the production-hardening changes for per-database
  runtime state, response caching, egress allowlisting, cancellation-aware
  provider calls, retry backoff, yyjson-backed response parsing, and bounded
  intra-chunk provider fan-out.
- No live Ollama or remote-provider smoke was run for this release candidate
  because no binaries are being published.

## 2026-07-01 macOS local validation

Environment:

- Platform: macOS 26.3, Darwin 25.3.0, arm64
- DuckDB submodule: `08e34c44`
- extension-ci-tools submodule: `b777c70`
- CMake: `4.3.4`
- Ninja: `1.13.2`

Commands:

```sh
PATH=/tmp/duckdb_ai_format_venv/bin:$PATH GEN=ninja make format-check
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```

Result:

- Release build passed and linked `extension/duckdb_ai/duckdb_ai.duckdb_extension`.
- Format check passed after running the repository formatter.
- SQLLogic tests passed with 136 assertions.
- Mock provider smoke passed.

Coverage:

- `duckdb_ai` extension naming across the build target, loadable extension, and
  SQLLogic load path.
- Canonical `TYPE duckdb_ai` secret support through the common `AI_PROVIDER`
  secret parameter, redacted discovery through `ai_secrets()`, and aliases for
  Anthropic, GCP/Gemini, Azure, Databricks, OpenAI Privacy Filter, and
  local/OpenAI-compatible providers.
- The existing broad SQL function catalog, including request builders, provider
  metadata helpers, task wrappers, SQL-assistant helpers, embeddings,
  aggregates, usage, and model metadata.
- Family-specific default model settings for completion, task, aggregate, SQL
  assistant, and embedding function groups.
- Mock HTTP coverage for OpenAI-compatible completions/embeddings/logging,
  Ollama chat, Claude messages, Databricks chat, Snowflake Cortex REST chat,
  OpenAI Privacy Filter redaction, retries, usage events, and provider error
  redaction.

Not covered:

- Real Ollama or remote provider live smoke.
- Signed binary publishing.

## 2026-06-30 macOS local validation

Environment:

- Platform: macOS 26.3, Darwin 25.3.0, arm64
- DuckDB submodule: `08e34c44`
- extension-ci-tools submodule: `b777c70`
- CMake: `4.3.4`
- Ninja: `1.13.2`

Commands:

```sh
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```

Result:

- Release build passed.
- SQLLogic tests passed with 116 assertions.
- Mock provider smoke passed.

Coverage:

- Static and loadable extension build on macOS arm64.
- Deterministic SQLLogic coverage for request builders, provider metadata,
  schema context, validation errors, model pricing catalog rows, and
  settings/named-option bounds.
- Mock HTTP coverage for OpenAI-compatible completions/embeddings/logging,
  Ollama chat, Claude messages, retries, usage events, provider error
  redaction, expanded JSON Schema validation, explicit cost estimates, and
  opt-in built-in catalog cost estimates.

Notes:

- After expanding local JSON Schema enforcement for string, numeric, array,
  object, and composition constraints, this macOS release/test/smoke gate was
  rerun successfully. After adding `ai_complete_record` and then nested
  STRUCT/LIST projection, the same gate was rerun again successfully. After
  adding the built-in model pricing catalog and opt-in catalog-backed cost
  estimation, the gate was rerun again successfully. SQLLogic now has 116
  assertions; the mock provider smoke covers valid constrained JSON, typed
  record projection, nested object/list projection, local length, numeric,
  array, and `oneOf` validation failures, and built-in pricing cost estimates.

Not covered:

- Real Ollama or remote provider live smoke.
- Signed binary publishing.

## 2026-06-30 Linux container validation

Environment:

- Container: Ubuntu 24.04.4 LTS through Docker/Colima, `--platform linux/arm64`
- Platform: Linux 6.8.0-100-generic, aarch64
- DuckDB submodule: `08e34c44`
- extension-ci-tools submodule: `b777c70`
- CMake: `3.28.3`
- Ninja: `1.11.1`

Commands:

```sh
CMAKE_BUILD_PARALLEL_LEVEL=1 GEN=ninja make release
CMAKE_BUILD_PARALLEL_LEVEL=1 GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```

Result:

- Release build passed and linked `extension/duckdb_ai/duckdb_ai.duckdb_extension`.
- SQLLogic tests passed with 116 assertions.
- Mock provider smoke passed.

Notes:

- The default parallel Linux container build was OOM-killed under the local
  Colima memory cap while compiling DuckDB core functions. Re-running with
  `CMAKE_BUILD_PARALLEL_LEVEL=1` passed.
- The Linux gate was rerun after expanded JSON Schema validation,
  `ai_complete_record`, nested STRUCT/LIST projection, and built-in pricing
  catalog changes. It passed from a clean temporary validation copy.

Coverage:

- Static and loadable extension build on Linux arm64.
- Deterministic SQLLogic coverage for the same suite as macOS.
- Mock HTTP coverage for OpenAI-compatible completions/embeddings/logging,
  Ollama chat, Claude messages, retries, usage events, provider error
  redaction, expanded JSON Schema validation, typed record projection, nested
  object/list projection, explicit cost estimates, and opt-in built-in catalog
  cost estimates.

Not covered:

- Linux amd64 build/test validation.
- Real Ollama or remote provider live smoke.
- Signed binary publishing.
