# Changelog

All notable changes to `duckdb_ai` are documented here.

This project uses semantic versioning. Before `1.0.0`, minor versions may
include SQL API changes and patch versions should preserve the SQL API.

## 0.8.0 - 2026-10-08

This release refreshes default models and built-in prices for current provider
catalogs. SQL functions, options and result shapes are unchanged. Calls that
rely on a provider's default model now use a different model; pass
`model := ...` or set `MODEL` in a secret to keep the previous one.

### Added

- Built-in pricing and request compatibility for Claude Haiku 5.5
  (`claude-haiku-5-5`), Gemini 3.8 Flash (`gemini-3.8-flash`, with Google's
  introductory rate through 2026-12-31), DeepSeek V4.1 Flash (`deepseek-flash`),
  Grok 4.7, Cloudflare Clef-flash, Perplexity `pplx-decider-v1.1-27b` and
  OpenRouter `anthropic/claude-haiku-5.5`.

### Changed

- New default models:
  - `anthropic`: `claude-haiku-5-5` (was `claude-haiku-4-5`). It costs a tenth
    as much, uses adaptive thinking by default and counts about 30% more tokens
    for the same text.
  - `gemini`: `gemini-3.8-flash` (was `gemini-3.7-flash`), at the same price.
  - `vertex`: `google/gemini-3.8-flash` (was `google/gemini-2.5-flash`, which
    Vertex retires in October 2026).
  - `deepseek`: `deepseek-flash` (was `deepseek-v4-flash`, retired by DeepSeek
    and temporarily routed to `deepseek-flash`).
  - `azure`: `gpt-5.6-luna` (was `gpt-4o`, which Azure no longer offers to new
    deployments).
  - `fireworks`: `accounts/fireworks/models/gpt-oss-120b` (was `gpt-oss-20b`,
    removed from Fireworks serverless).
  - `nebius`: `Qwen/Qwen3-30B-A3B-Instruct-2507` (was
    `meta-llama/Meta-Llama-3.1-70B-Instruct`, no longer served).
  - `mimo`: `mimo-v2.6-flash` (was `mimo-v2.5-pro`, which retires on 2026-10-21).
  - `qianfan`: `ernie-5.1` (was `ernie-4.5-turbo-128k`, which retires on
    2026-10-29).
  - `ai_decide` with `perplexity`: `pplx-decider-v1.1-27b` (was
    `pplx-decider-v1-27b`).

### Fixed

- Built-in cost estimates use current rates: DeepSeek Flash at $0.30/$1.20 per
  million tokens (was $0.44/$1.32), Perplexity decision models at $0.02 per
  million input tokens (was $0.04), and Claude Sonnet 5.5 cache reads at 5% of
  the input rate (was 10%).
- `temperature` is omitted for models that reject non-default sampling
  parameters: Claude Haiku 5.5, Gemini 3.8 Flash, Gemini 3.x models on Vertex,
  and Claude 5.5-generation and Fable 5.1 endpoints on Databricks. Previously
  these requests could fail when `temperature` was set.

### Documentation

- Provider guides list the new defaults and note that Together no longer serves
  embeddings serverlessly, so its default embedding model needs a dedicated
  endpoint.

## 0.7.0 - 2026-10-06

### Added

- `ai_decide(state, questions, provider := ...)` answers typed questions with
  any supported decision model and returns the same typed `STRUCT` as `ai_jev`.
  It supports TypeSafe Jev, Cloudflare Clef and Clef-flash on Workers AI,
  Perplexity `pplx-decider-v1-27b`, local Ollama decision models such as
  `nimble` and `tev1`, and any `/v1/systemone` endpoint through the new
  `systemone` provider. Questions can carry their own `instructions`.
  `ai_jev` is unchanged and remains available as the TypeSafe-only form.

### Documentation

- New [chain AI steps](docs/cookbooks/chain-ai-steps.md) cookbook and agent
  guide section. They cover calling each model function once per row,
  skipping failed and NULL rows downstream, branching with `CASE`, and
  checkpointing each step so reruns only retry missing rows. A new smoke test
  pins these call counts and runs the cookbook SQL.
- Reworked docs for readers and agents: one documented order for how call
  options, profiles, session settings and secrets combine; `ai_decide` model
  resolution; missing providers (TypeSafe, systemone, Xiaomi MiMo) and aliases
  in the provider tables; and fixes to examples that failed as written.
- New cookbooks for routing rows by decision confidence and for evaluating a
  prompt or model on a labeled sample before a batch run.
- Function and setting descriptions shown by `duckdb_functions()`,
  `duckdb_settings()` and the community extension page now say what each one
  does and use consistent examples.

### Maintenance

- Build and test against DuckDB 1.5.6 and matching extension CI tooling. This
  includes the upstream fix for LIMIT/OFFSET pushdown across volatile
  projections, preserving evaluation order for model-backed SQL expressions.
  The sanitizer host and platform/distribution builds use the same version.
  OFFSET can evaluate model calls for skipped rows; paginate in an input
  subquery when pagination should happen before inference.

### Fixed

- Functions accept DuckDB native types and the usual ways of passing values:
  - Text inputs accept any type (numbers, dates, UUIDs, lists, structs, whole
    rows), sent as their text form.
  - A column or macro parameter in a positional slot is read as the per-row
    model, provider or instruction instead of being rejected as an unknown
    named option.
  - Prepared-statement parameters and macro parameters work for named options,
    JSON Schemas, `ai_decide` questions and classifier labels.
  - Labels accept fixed-size arrays, JSON-array strings and a `MAP` of label to
    description. Previously these were split into wrong labels.
  - `ai_decide` criteria accept any key, value or level type that casts to text,
    such as `MAP {true: ..., false: ...}` or `VARCHAR[3]` levels.
  - JSON options (`response_schema`, `request_options`, `metadata`,
    `label_descriptions`, `examples`) accept a `STRUCT`, `MAP` or list.
  - `include_tables` and `exclude_tables` accept a single table name.
  - A NULL named option keeps the default instead of raising an error.
- `ai_generate_chunks` and `ai_prep_search` can chunk a column with a lateral
  join (`FROM docs d, ai_generate_chunks(d.body)`).
- `ai_agg(..., instruction := ...)` and `task := ...` no longer fail with
  "requires a constant instruction argument".
- An empty label list, a NULL label or duplicate labels now raise an error
  (or follow `on_error`) before any request is sent, instead of returning NULL
  or paying for a request.
- `ai_extract_record` and `ai_complete_record` reject JSON Schemas whose
  property names are empty or differ only in case, instead of building a
  `STRUCT` that cannot be stored.
- `ai_decide` no longer sends the chat model stored in an Ollama, Cloudflare or
  Perplexity secret to the decision endpoint, and TypeSafe decisions no longer
  pick up a chat model from `DUCKDB_AI_MODEL`. `TYPESAFE_DECISION_MODEL` is
  accepted alongside `TYPESAFE_MODEL`.
- Model replies are read the way small and local models actually write them:
  - `ai_classify`, `ai_classify_labels` and `ai_classify_result` accept a label
    wrapped in markdown or quotes, after a `Label:` prefix, with trailing
    punctuation or followed by an explanation. Multi-label replies may be
    unquoted (`[billing, other]`), a bare label or a bullet list, and repeated
    labels are de-duplicated instead of failing.
  - `ai_filter` accepts `True.`, `**Yes**`, `"false"` and an answer followed by
    a reason. `ai_rerank` accepts `Score: 0.8`, `0.8 - relevant` and
    `{"score": 0.8}`, and no longer reads hex such as `0x1p-1` as a score.
  - `ai_complete_json`, `ai_extract_record` and `ai_complete_record` find the
    JSON inside a code fence or after a sentence. `ai_extract` drops a code
    fence, and text outputs no longer keep leading or trailing whitespace.
  - A leading `<think>` reasoning block is removed. A reply cut off by
    `max_tokens`, or one that contains only reasoning, now raises an error (or
    follows `on_error`) instead of returning partial text.
  - `ai_sql`, `ai_query_data` and `ai_fix_sql` find the query after prose,
    inside a one-line fence or after a `SQL:` label, and ignore text after it.
  - `ai_decide` matches a choice that differs only in case or surrounding
    spaces, and returns NULL for `"confidence": null`.
- JSON Schemas map to the expected column types: local `$ref`/`$defs` (as
  generated by pydantic), `anyOf`/`oneOf` with one non-null branch
  (`Optional[int]` becomes `BIGINT`), and `["integer", "number"]` becomes
  `DOUBLE`. Properties with several types become `VARCHAR` instead of failing
  on valid values.
- Deeply nested JSON replies (more than 512 levels) return an error instead of
  crashing the process.
- Usage rows keep the provider's token counts when a reply fails validation,
  record the reason when `ai_query_data(..., on_error := 'null')` swallows an
  error, store plain error messages, and report Ollama's cached prompt tokens.
- `ai_fix_sql` in line mode rejects an error that points past the last line of
  the query instead of accepting any reply. Errors name the function that
  failed (`ai_extract_record`, not `ai_complete_record`).
- Classifier centroid accumulation avoids overflowing finite embeddings. Labels
  preserve commas, escaped characters, literal quotes, whitespace, and fences
  through training and fallback; ambiguous duplicate labels fail early.
- Generated SQL cache keys include resolved credentials and model profiles, so
  credential rotation and profile replacement invalidate earlier entries.
- Completion token reservations and input limits include system prompts and
  response schemas.
- Release signing uses private temporary staging with failure cleanup. Unsigned
  reruns reset signatures; unsupported signature sizes fail explicitly, and
  WebAssembly compression and upload headers agree for every wasm target.

### Changed

- `ai_usage()` returns NULL instead of `-1` for counts the provider did not
  report (tokens, cached tokens, character counts, dimensions) and for
  `http_status` when no HTTP call was made, so `sum()` and `avg()` are correct.
  `ai_usage_summary()` returns NULL `total_tokens` and `estimated_cost_usd` when
  no call in the group reported them. Replace `col < 0` checks with
  `col IS NULL`.
- Character counts in `ai_usage()` count Unicode characters instead of bytes.
- `ai_sentiment` always returns `positive`, `neutral` or `negative`. Other
  replies raise an error or follow `on_error`.
- `STRUCT` fields from `ai_extract_record` and columns from
  `ai_complete_record` follow the order of the JSON Schema properties instead
  of alphabetical order. Select fields by name rather than position.

### Performance

- Generated SQL cache recency updates take constant time. Retained key and SQL
  text is bounded by 64 MiB as well as the existing 1,024-entry limit.

### Diagnostics and validation

- Added `ai_query_cache_stats()` and `ai_usage_totals()` without changing existing
  result schemas. Resumable enrichment persists batch counters with its results.
- Classifier sampling now selects distinct texts across the entire relation using
  stable bottom-k priorities, with identical texts isolated from holdout leakage.
- Model profiles support conservative `token_estimate_multiplier` margins.
  `context_size` reserves output as well as input; `max_input_tokens` remains an
  independent input-only limit.
- Accuracy thresholds and required-label gates are available in the labeled
  evaluator, with independent multilingual synthetic fixtures in CI.
- CI now exercises owned C++ ASan/UBSan, deterministic schema/chunk fuzz cases,
  shared-connection stress, and macOS/Windows extension checks on every change.

### Evaluation

- The labeled Jev evaluator reports per-class precision, recall, F1, support,
  macro F1, answer coverage, and a confusion matrix including failed predictions.

## 0.6.1 - 2026-10-01

### Fixed

- Typed completion validation now runs before success accounting and evicts
  rejected cached responses. Record projection preserves exact 64-bit integers
  and respects `fail_on_error` for projection failures.
- JSON Schema validation now enforces union types and nested boolean schemas,
  counts Unicode code points, compares decimal lexemes exactly, and validates
  integer divisibility without narrowing large values to 64 bits.
- Cosine similarity handles extreme finite embedding magnitudes, and cached
  embeddings retain double precision.
- Classifier holdouts retain training examples for each observed class; artifacts
  without validation examples are marked unusable.
- Batch-size recommendations reject results outside the BIGINT range.

### Performance

- Chunk metadata advances through the document instead of rescanning every
  prefix. Usage-buffer eviction now takes constant time, and per-request model
  pricing avoids copying the complete price catalog.

### Maintenance

- CI now analyzes extension sources directly under `src/` with clang-tidy.
- Added deterministic regressions and updated three documentation dependencies
  to versions without currently reported npm audit vulnerabilities.

## 0.6.0 - 2026-09-30

### Added

- `ai_jev(text, questions)` returns named choices, rubric scores, probabilities
  and confidence as typed SQL fields. It batches up to 32 rows per request,
  preserves row identity, and keeps existing Jev entry points unchanged.
  [#101](https://github.com/leonardovida/duckdb-ai/pull/101)

- A labeled-data Jev batch evaluation example compares batch sizes 1, 8, 16 and
  32 with row-level predictions, accuracy, agreement coverage, request
  operations and token usage. [#104](https://github.com/leonardovida/duckdb-ai/pull/104)

- Built-in pricing and request compatibility now cover GPT-6 Astra, GPT-6.1
  Sol, GPT-6 Sol, GPT-6 Luna, Claude Fable 5.1, Claude Opus 5.5 and Claude
  Sonnet 5.5. [#112](https://github.com/leonardovida/duckdb-ai/pull/112)

### Fixed

- Numeric `DUCKDB_AI_*` environment settings now reject valid prefixes followed
  by trailing text, so values such as `0junk` cannot silently change retry,
  cache, concurrency, token-limit or logging behavior.
  [#106](https://github.com/leonardovida/duckdb-ai/pull/106)

- Provider cache reads and writes now contribute to token totals and cost
  estimates using model-specific rates. Unsupported sampling parameters are
  omitted for the new reasoning models while GPT-6 Sol and Luna retain their
  temperature behavior with reasoning disabled.
  [#112](https://github.com/leonardovida/duckdb-ai/pull/112)

### Changed

- Added shared provider response-format validation for no-schema requests while
  preserving provider-specific JSON and schema-first behavior.
  [#99](https://github.com/leonardovida/duckdb-ai/pull/99)

- Consolidated usage snapshots, usage statistics, usage clearing and response
  cache clearing around shared database-scoped runtime state operations.
  [#100](https://github.com/leonardovida/duckdb-ai/pull/100)

- Reused successful `patternProperties` matches when validating
  `additionalProperties`, keeping validation order and SQL behavior stable.
  [#110](https://github.com/leonardovida/duckdb-ai/pull/110)

- Shared scalar and cached batch embedding validation and rejected-response
  cleanup while preserving diagnostics, cache identity and bulk-request
  behavior. [#111](https://github.com/leonardovida/duckdb-ai/pull/111)

### Maintenance

- CI now runs code, test, example, build, documentation and website checks only
  for the paths they cover, while retaining manual and scheduled runs.
  [#105](https://github.com/leonardovida/duckdb-ai/pull/105)

- Patched the documentation dependency versions for vulnerable `colord` and
  `image-size` releases through the existing npm overrides.
  [#107](https://github.com/leonardovida/duckdb-ai/pull/107)

## 0.5.2 - 2026-09-26

### Fixed

- Preserve every Anthropic Messages text block in completion results, including
  responses with interleaved thinking blocks. [#108](https://github.com/leonardovida/duckdb-ai/pull/108)

## 0.5.1 - 2026-09-24

### Fixed

- Accept CR-only line endings in native provider SSE responses, alongside LF
  and CRLF framing. [#102](https://github.com/leonardovida/duckdb-ai/pull/102)

## 0.5.0 - 2026-09-18

### Added

- TypeSafe Jev evaluation provider with native multi-question requests,
  `ai_classify` and `ai_filter` adapters, model pricing, and examples for ticket
  triage, reranking, entity matching, and model routing. [#97](https://github.com/leonardovida/duckdb-ai/pull/97)
- Native JSON provider calls for reasoning and tool-state exchange, buffered SSE,
  embeddings, and reranking through explicit service endpoints. Added guarded
  provider-native completion options and MiMo/Xiaomi credentials and defaults.
  [#94](https://github.com/leonardovida/duckdb-ai/pull/94)
- Resumable local enrichment with durable checkpoints, selective retries, and
  prompt/configuration invalidation. [#95](https://github.com/leonardovida/duckdb-ai/pull/95)

### Fixed

- Escape multiline text and control characters in JSON requests and reject
  malformed Jev decisions and unsupported generation options.
  [#97](https://github.com/leonardovida/duckdb-ai/pull/97)

### Changed

- Share embedding-job preparation without changing SQL behavior.
  [#90](https://github.com/leonardovida/duckdb-ai/pull/90)
- Clarify agent-first setup, document native text API coverage, and use Qwen3.8
  27B in local examples. [#91](https://github.com/leonardovida/duckdb-ai/pull/91)
  [#94](https://github.com/leonardovida/duckdb-ai/pull/94)
- Patch documentation dependencies for SVGO, SWC HTML, YAML merge-budget, and
  Joi prototype-handling advisories. [#92](https://github.com/leonardovida/duckdb-ai/pull/92)
  [#93](https://github.com/leonardovida/duckdb-ai/pull/93)
  [#96](https://github.com/leonardovida/duckdb-ai/pull/96)

No breaking SQL API changes are expected. Jev is a decision model and does not
support generated text or embeddings. Native tool calls are never executed by the
extension. Jev latency and prediction quality have not been measured live.

## 0.4.25 - 2026-09-04

### Fixed

- Updated DeepSeek V4 Flash built-in token estimates to the current peak-hour
  cache-miss rates, while documenting that off-peak billing is 50% lower.
- Replaced NVIDIA NIM's deprecated free-endpoint default with the supported
  `nvidia/nemotron-3-super-120b-a12b` model.

## 0.4.24 - 2026-09-04

### Fixed

- Accepted Databricks reasoning-model responses with typed text content blocks
  and omitted unsupported sampling temperature for Claude Sonnet 5 and Opus 5
  model services, fixing the completion failures reported in #85.
- Updated the documentation lockfile to patched `browserslist`, `fast-uri`, and
  `qs` releases, resolving their memory-growth, crash, URL-confusion, SSRF, and
  query-parser advisories.

## 0.4.23 - 2026-08-28

### Changed

- Updated the xAI / SpaceXAI completion default to `grok-4.6`, while retaining
  explicit `grok-4.5` selection and pricing metadata.

### Fixed

- Kept Claude Sonnet 5 built-in cost estimates at Anthropic's permanent
  $2/M input and $10/M output rates instead of applying the canceled September
  price increase.

## 0.4.22 - 2026-08-28

### Fixed and performance

- Honored embedding response indexes when mapping batched provider results back
  to inputs, preventing out-of-order OpenAI-compatible responses from silently
  assigning vectors to the wrong rows. Malformed indexes now fail explicitly,
  while gateways that omit indexes remain compatible.
- Replaced quadratic JSON Schema `uniqueItems` validation with hash-bucketed
  structural comparison, while preserving exact numeric, array, and object
  equality checks.

## 0.4.21 - 2026-08-26

### Fixed

- Corrected GPT-5.6 Sol built-in token prices and applied the same rates to the
  `gpt-5.6` alias so opt-in usage-cost estimates match current OpenAI billing.

## 0.4.20 - 2026-08-21

### Fixed

- Corrected Anthropic's built-in standard API prices after discounted Batch API
  rates were applied to opt-in usage-cost estimates by mistake.

## 0.4.19 - 2026-08-21

### Changed

- Updated Gemini's completion default to stable `gemini-3.7-flash` and omitted
  sampling parameters that Google no longer accepts for this model.

### Fixed

- Applied Google's introductory Gemini 3.7/3.6 Flash prices through December
  31, 2026, with an automatic rollover to the published standard rate.
- Updated the documentation lockfile to NanoID 3.3.18, resolving its
  zero-length custom-generator denial-of-service advisory.

## 0.4.18 - 2026-08-18

### Fixed

- Corrected the built-in Anthropic prices for Claude Haiku 4.5, Sonnet 4.5,
  and Sonnet 5 so opt-in usage-cost estimates match current Claude API rates.
- Updated built-in GPT-5.6 Terra and Luna token prices after OpenAI reduced
  their API prices.

### Removed

- Removed GitHub Models provider defaults, aliases, credentials, and docs
  because GitHub fully retired the inference API on July 30, 2026.

## 0.4.17 - 2026-08-09

### Fixed

- Replaced Fireworks AI's non-serverless completion default with the supported
  `accounts/fireworks/models/gpt-oss-20b` serverless model.
- Updated the documentation lockfile to patched `fast-uri`, `js-yaml`, `nanoid`,
  and `postcss` releases, resolving all dependency advisories with published
  fixes.

### Changed

- Updated the OpenAI completion default from `gpt-4o-mini` to
  `gpt-5.6-luna` for stronger quality at a cost-efficient GPT-5.6 tier. Explicit
  per-call, secret, environment, and session model overrides remain unchanged.

## 0.4.16 - 2026-07-31

### Fixed

- Replaced Together AI's retired default embedding model with the current
  `intfloat/multilingual-e5-large-instruct` serverless model.
- Rejected structured-output options for Poe Chat Completions instead of
  sending a `response_format` field that Poe ignores.
- Marked stable OpenAI GPT-5.6 system-message prefixes with explicit prompt
  cache breakpoints, preventing changing row prompts from causing unnecessary
  billable cache writes.
- Updated the documentation lockfile to `brace-expansion` 5.0.9 and
  `@types/react` 19.2.18, resolving the remaining dependency audit advisory.

## 0.4.15 - 2026-07-30

### Changed

- Updated the local DuckDB and extension CI tooling to the 1.5.5 bugfix,
  performance, and security patch release.
- Updated Gemini's completion default and built-in pricing to stable
  `gemini-3.6-flash`, omitting sampling parameters that the model deprecates.
- Updated Kimi's default completion model to `kimi-k3`.
- Stopped injecting a generic sampling temperature when callers omit it, so
  model-specific provider defaults remain valid across Kimi, Fireworks, and
  other OpenAI-compatible catalogs.

### Fixed

- Sent JSON Schema completion requests to Anthropic through the current
  `output_config.format` structured-output shape instead of rejecting them.
- Corrected the Amazon Bedrock Mantle default model ID for OpenAI-compatible
  Chat Completions requests.
- Restored StepFun's direct API default to the documented `step-3.5-flash`
  model and canonical `https://api.stepfun.com/v1` endpoint.
- Refreshed the documentation lockfile to resolve the `body-parser` size-limit
  advisory without changing direct package ranges.
- Updated the documentation lockfile to React 19.2.8 and patched `fast-uri`
  3.1.4, resolving its URI validation denial-of-service advisory.

## 0.4.14 - 2026-07-21

### Fixed

- Discovered portable system CA bundle paths for HTTPS provider requests and
  honored standard certificate environment overrides, fixing provider access
  from slim Linux containers.

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
