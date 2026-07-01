# duckdb_ai

`duckdb_ai` is a DuckDB extension that lets SQL call AI model providers.
It is based on [`duckdb/extension-template`](https://github.com/duckdb/extension-template).

The first implementation slice includes:

- `ai_complete(prompt[, model[, provider]])`: calls a configured AI provider and returns the completion text.
- `ai_complete_json(prompt[, model[, provider]])`: asks for JSON-only output,
  sends structured-output hints to providers that support them, validates the
  returned text as a top-level JSON object or array, and returns it as `VARCHAR`.
- `ai_complete_record(prompt, response_schema[, model[, provider]])`: table
  function that asks for JSON, validates it against a top-level object schema,
  and projects declared schema properties as typed DuckDB columns.
- `ai_request_json(prompt[, model[, provider]])`: returns the provider request body without making a network call.
- `ai_embed(text[, model[, provider]])`: calls a configured embedding provider and returns a `DOUBLE[]`.
- `ai_embedding_request_json(text[, model[, provider]])`: returns the embedding request body without making a network call.
- Task wrappers over completion models: `ai_summarize`, `ai_sentiment`,
  `ai_fix_grammar`, `ai_redact`, `ai_translate`, `ai_classify`, and
  `ai_extract`.
- Aggregate wrappers over completion models: `ai_summarize_agg(text)` and
  `ai_agg(text, instruction)`, with constant model/provider/options evaluated
  once per aggregate group.
- `ai_similarity(left_text, right_text[, model[, provider]])`: embeds two
  strings and returns cosine similarity.
- `ai_is_read_only_sql(sql)` and `ai_validate_read_only_sql(sql)`: deterministic
  parser gates for generated SQL; currently only a single `SELECT` statement is
  accepted.
- `ai_sql(question[, schema_context[, model[, provider]]])`: asks a model
  for one DuckDB `SELECT` statement, strips common markdown code fences, and
  rejects anything that does not pass the read-only parser gate. When
  `schema_context` is omitted, the extension supplies local catalog context from
  `ai_schema_prompt()`.
- `ai_schema_prompt([include_tables])`: returns one `summary` row with deterministic
  DuckDB catalog context for the current database. `include_tables` and
  `exclude_tables` accept table, `schema.table`, or `catalog.schema.table`
  names. `sample_rows` includes bounded per-table sample rows for local DuckDB
  tables when the context is built during execution, and otherwise emits safe
  sample query hints for bind-time SQL-assistant paths.
- `ai_query_data(question[, schema_context[, model[, provider]]])`: table
  function that generates one read-only DuckDB `SELECT` at bind time and runs it
  as a subquery. It also defaults to `ai_schema_prompt()` context when
  `schema_context` is omitted.
- `ai_explain_sql(sql[, ...])`: table function that explains one read-only
  DuckDB `SELECT` statement using optional schema context.
- `ai_fix_sql(sql[, ...])`: table function that asks the model to rewrite a
  broken query as one corrected read-only DuckDB `SELECT` statement.
- `ai_fix_sql_line(sql[, error][, ...])`: table function that asks the model to
  rewrite only the line identified by an error message.
- Completion named options: `model`, `provider`, `temperature`,
  `system_prompt`, `max_tokens`, `base_url`, `timeout_seconds`,
  `retry_count`, `retry_backoff_ms`, `fail_on_error`, `secret`,
  `response_format`, `response_schema`, `max_concurrent_requests`,
  `min_request_interval_ms`, `input_token_price_per_million`,
  `output_token_price_per_million`, `use_builtin_model_prices`, `log_tags`, and
  `log_sample_rate`.
- Embedding named options: `model`, `provider`, `base_url`,
  `timeout_seconds`, `retry_count`, `retry_backoff_ms`, `secret`,
  `fail_on_error`, `max_concurrent_requests`, `min_request_interval_ms`,
  `input_token_price_per_million`, `use_builtin_model_prices`, `log_tags`, and
  `log_sample_rate`.
- `ai_provider_base_url(provider)`: returns the default base URL for a supported provider.
- `ai_provider_protocol(provider)`: returns the internal request protocol for a supported provider.
- `ai_usage()`: returns recent in-process AI completion and embedding usage events.
- `ai_clear_usage()`: clears the in-process usage event buffer.
- `ai_secrets()`: lists configured `duckdb_ai` secrets with credentials redacted.
- `ai_models()`: returns the small built-in provider/model pricing catalog
  used by opt-in cost estimation.

Supported providers in this slice:

| Provider | Protocol | Default base URL | API key environment variable |
| --- | --- | --- | --- |
| `ollama` | `ollama_chat` | `http://localhost:11434` | optional `OLLAMA_API_KEY` |
| `openai` | `openai_chat` | `https://api.openai.com/v1` | `OPENAI_API_KEY` |
| `azure` | `openai_chat` | set `BASE_URL` or `AZURE_OPENAI_BASE_URL`; `/openai/v1` is appended when needed | `AZURE_OPENAI_API_KEY` |
| `claude` / `anthropic` | `anthropic_messages` | `https://api.anthropic.com/v1` | `ANTHROPIC_API_KEY` |
| `gemini` / `gcp` / `google` | `openai_chat` | `https://generativelanguage.googleapis.com/v1beta/openai` | `GEMINI_API_KEY` |
| `mistral` | `openai_chat` | `https://api.mistral.ai/v1` | `MISTRAL_API_KEY` |
| `zai` | `openai_chat` | `https://open.bigmodel.cn/api/paas/v4` | `ZAI_API_KEY` |
| `deepseek` | `openai_chat` | `https://api.deepseek.com` | `DEEPSEEK_API_KEY` |
| `openrouter` | `openai_chat` | `https://openrouter.ai/api/v1` | `OPENROUTER_API_KEY` |
| `openai_compatible` / `local` | `openai_chat` | set `BASE_URL` for the gateway, e.g. Ollama `/v1`, vLLM, LM Studio, or LiteLLM | optional `OPENAI_COMPATIBLE_API_KEY` |

Provider-specific values can be overridden with `<PROVIDER>_BASE_URL`,
`<PROVIDER>_MODEL`, and `<PROVIDER>_API_KEY`. Generic overrides are
`DUCKDB_AI_PROVIDER`, `DUCKDB_AI_MODEL`, `DUCKDB_AI_BASE_URL`, and
`DUCKDB_AI_API_KEY`.

DuckDB settings can also provide session defaults:

```sql
SET duckdb_ai_provider = 'openai';
SET duckdb_ai_model = 'gpt-4o-mini';
SET duckdb_ai_base_url = 'https://api.openai.com/v1';
SET duckdb_ai_timeout_seconds = 120;
SET duckdb_ai_retry_count = 0;
SET duckdb_ai_retry_backoff_ms = 1000;
SET duckdb_ai_max_concurrent_requests = 4;
SET duckdb_ai_min_request_interval_ms = 100;
SET duckdb_ai_response_format = 'json_object';
SET duckdb_ai_input_token_price_per_million = 0.15;
SET duckdb_ai_output_token_price_per_million = 0.60;
SET duckdb_ai_use_builtin_model_prices = true;
SET duckdb_ai_log_endpoint = 'https://collector.example/ai-usage';
SET duckdb_ai_log_format = 'generic_json';
SET duckdb_ai_log_tags = 'warehouse=local,app=duckdb';
SET duckdb_ai_log_sample_rate = 0.25;
```

## Usage

```sql
LOAD duckdb_ai;

SELECT ai_request_json('summarize this text', 'gpt-4o-mini', 'openai');
SELECT ai_request_json(
    'summarize this text',
    provider := 'openai',
    model := 'gpt-4o-mini',
    system_prompt := 'answer tersely',
    temperature := 0.2,
    max_tokens := 128,
    timeout_seconds := 30
);
SELECT ai_complete('write one sentence about DuckDB', 'llama3.2', 'ollama');
SELECT ai_complete(
    'write one sentence about DuckDB',
    provider := 'openai',
    model := 'gpt-4o-mini',
    fail_on_error := false
);
CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    API_KEY '...',
    AI_PROVIDER 'openai'
);
SELECT ai_complete(
    'write one sentence about DuckDB',
    secret := 'openai_ai'
);
SELECT ai_request_json(
    'extract a summary',
    provider := 'openai',
    response_format := 'json_object'
);
SELECT ai_complete_json(
    'extract a compact company profile for DuckDB',
    provider := 'openai',
    response_schema := '{"type":"object","properties":{"name":{"type":"string"},"summary":{"type":"string"}},"required":["name","summary"],"additionalProperties":false}'
);
SELECT *
FROM ai_complete_record(
    'extract a compact company profile for DuckDB',
    '{"type":"object","properties":{"name":{"type":"string"},"score":{"type":"number"},"tags":{"type":"array","items":{"type":"string"}}},"required":["name"],"additionalProperties":false}',
    provider := 'openai'
);
SELECT ai_embed(
    'DuckDB is an analytical database',
    provider := 'openai',
    model := 'text-embedding-3-small'
);
SELECT ai_similarity('DuckDB analytics', 'analytical SQL database');
SELECT ai_summarize('DuckDB is an analytical database built for fast local queries.');
SELECT ai_translate('hello', 'Dutch');
SELECT ai_classify('invoice overdue', 'billing, support');
SELECT ai_extract('name: DuckDB', 'name');
SELECT customer_id, ai_summarize_agg(note ORDER BY created_at)
FROM support_notes
GROUP BY customer_id;
SELECT ai_agg(message, 'List the top three recurring themes')
FROM customer_feedback;
SELECT summary
FROM ai_schema_prompt(
    include_tables := ['main.orders'],
    exclude_tables := ['main.internal_audit'],
    sample_rows := 3
);
SELECT ai_sql(
    'count orders by status'
);
SELECT *
FROM ai_query_data(
    'count orders by status',
    schema_context := (SELECT summary FROM ai_schema_prompt(include_tables := ['main.orders']))
);
SELECT explanation
FROM ai_explain_sql(
    'SELECT status, count(*) FROM orders GROUP BY status',
    include_tables := ['main.orders'],
    sample_rows := 3
);
SELECT sql
FROM ai_fix_sql('SEELECT status, count(*) FRUM orders GROUP BY status');
SELECT line_number, replacement_line
FROM ai_fix_sql_line(
    'SEELECT status FROM orders',
    error := 'Parser Error: syntax error at or near "SEELECT" LINE 1'
);
SELECT ai_validate_read_only_sql('SELECT count(*) FROM my_table');
SELECT * FROM ai_usage();
SELECT * FROM ai_secrets();
SELECT * FROM ai_models();
```

`base_url` is useful for local gateways and tests. API keys are resolved from
environment variables or DuckDB Secrets, not from direct function arguments.

DuckDB Secrets are also supported for credentials:

```sql
CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    API_KEY '...',
    MODEL 'gpt-4o-mini',
    BASE_URL 'https://api.openai.com/v1'
);

SELECT ai_complete('hello', secret := 'openai_ai');
```

`API_KEY` is redacted by `duckdb_secrets()`. `AI_PROVIDER` is required.
`MODEL` and `BASE_URL` are optional. If no explicit `SCOPE` is provided, the
extension scopes the secret to the normalized provider name so calls with
`provider := 'openai'` can discover it automatically.

Use the same secret shape for every provider:

```sql
CREATE OR REPLACE SECRET local_llm (
    TYPE duckdb_ai,
    AI_PROVIDER 'local',
    BASE_URL 'http://localhost:11434/v1',
    MODEL 'llama3.2'
);

CREATE OR REPLACE SECRET azure_llm (
    TYPE duckdb_ai,
    AI_PROVIDER 'azure',
    API_KEY '...',
    BASE_URL 'https://my-resource.openai.azure.com',
    MODEL 'gpt-4o'
);

SELECT name, provider, model, base_url, has_api_key
FROM ai_secrets();
```

`response_format` accepts `text`, `json_object`, or `json_schema`.
`response_schema` should be a JSON Schema object; when present, it implies
`json_schema` mode. OpenAI-compatible providers receive a native
`response_format` payload, and Ollama receives `format`. Claude has no native
payload field in this slice; use `ai_complete_json` for JSON-only prompting and
local validation. Local `ai_complete_json` validation enforces the common JSON
Schema keywords `type`, `properties`, `required`, `additionalProperties`,
`patternProperties`, `propertyNames`, `dependentRequired`, `minProperties`,
`maxProperties`, `items`, `minItems`, `maxItems`, `uniqueItems`, `contains`,
`minContains`, `maxContains`, `minimum`, `maximum`, `exclusiveMinimum`,
`exclusiveMaximum`, `multipleOf`, `minLength`, `maxLength`, `pattern`, `enum`,
`const`, `allOf`, `anyOf`, `oneOf`, and `not`.
This is the initial pragmatic local subset, not a claim of full JSON Schema
draft parity.

`ai_complete_record` uses the same validation path and projects supported
object schemas as DuckDB columns. Scalar properties become `VARCHAR`, `BOOLEAN`,
`BIGINT`, or `DOUBLE`; nested object schemas become `STRUCT`; supported arrays
become typed `LIST`s, including arrays of nested object schemas. Unsupported
schema shapes are returned as JSON text.

Retries are explicit and disabled by default. Set `retry_count := 2` or
`SET duckdb_ai_retry_count = 2` to retry curl failures and HTTP 429/5xx
responses. `retry_backoff_ms` controls the fixed delay between attempts.
Set `max_concurrent_requests := 4` to cap in-process provider HTTP calls, and
`min_request_interval_ms := 100` to enforce a minimum gap between provider
request starts. These controls apply to completion and embedding provider calls;
usage-log delivery is not counted against the provider limit.

For OpenAI-compatible providers, set the provider API key in the environment
before starting DuckDB:

```sh
export OPENAI_API_KEY='...'
```

For local Ollama, start Ollama first:

```sh
ollama serve
ollama pull llama3.2
```

## Usage logging

Set `DUCKDB_AI_LOG_ENDPOINT` to POST privacy-minimized usage events after a
successful completion or embedding request. The event includes provider, protocol, model,
prompt/response character counts for completions, dimensions for embeddings,
token counts when the provider returns them, elapsed milliseconds, and HTTP
status. It does not include prompt, input, or response text unless
`DUCKDB_AI_LOG_INCLUDE_TEXT=1` is set.

`DUCKDB_AI_LOG_FORMAT`, `SET duckdb_ai_log_format = ...`, or
`log_format := ...` controls the outbound payload shape. `generic_json` is the
default compact extension payload. `otlp_json` emits an OpenTelemetry Protocol
JSON logs envelope for collectors; point the endpoint at the collector's log
ingest path, usually `/v1/logs`.

Successful completions, embeddings, and local `ai_schema_prompt` calls are also
recorded in a local in-process ring buffer. Query it with
`SELECT * FROM ai_usage();`. The buffer keeps the latest 1,024 events and can be
cleared with `SELECT * FROM ai_clear_usage();`.

Cost estimates are disabled by default. Pass
`input_token_price_per_million := ...` and, for completions,
`output_token_price_per_million := ...`, or set the matching DuckDB settings, to
add `estimated_cost_usd` to usage events and logs when the provider returns token
counts.

You can also opt into the small built-in catalog with
`use_builtin_model_prices := true`,
`SET duckdb_ai_use_builtin_model_prices = true`, or
`DUCKDB_AI_USE_BUILTIN_MODEL_PRICES=1`. Explicit per-call or per-session
prices take precedence over catalog prices. Inspect the catalog with
`SELECT * FROM ai_models();`; each row includes the provider, model,
operation, USD-per-million-token prices, source URL, source note, and
`last_reviewed` date. Provider pricing changes often, so treat the built-in
catalog as a convenience for common models rather than a billing authority.

By default, logging failures do not fail the SQL query. Set
`DUCKDB_AI_LOG_STRICT=1` or `SET duckdb_ai_log_strict = true` to make log
delivery failures raise an error. `SET duckdb_ai_log_include_text = true`
has the same effect as `DUCKDB_AI_LOG_INCLUDE_TEXT=1`.

Attach lightweight labels with `log_tags := 'tenant=dev,job=nightly'`,
`SET duckdb_ai_log_tags = '...'`, or `DUCKDB_AI_LOG_TAGS`. Sample outbound
log delivery with `log_sample_rate := 0.1`,
`SET duckdb_ai_log_sample_rate = 0.1`, or `DUCKDB_AI_LOG_SAMPLE_RATE=0.1`.
Sampling is deterministic for the event provider, model, event type, and input
text; the default rate is `1.0`.

## Provider errors and redaction

Non-2xx provider responses are normalized into errors that include provider,
protocol, model, HTTP status, and provider error `type`/`code` when present.
The active API key is redacted before provider error bodies are surfaced.
Request-preview functions do not include credentials, and usage logs do not
include credentials.

## Building

Initialize submodules, then build:

```sh
git submodule update --init --recursive
make
```

The main artifacts are:

```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/duckdb_ai/duckdb_ai.duckdb_extension
```

See [`docs/UPDATING.md`](docs/UPDATING.md) for DuckDB submodule update notes,
[`docs/DISTRIBUTION.md`](docs/DISTRIBUTION.md) for the current CI and
distribution stance, and [`docs/RELEASING.md`](docs/RELEASING.md) for preview
and stable release/versioning gates. Current local platform evidence is recorded in
[`docs/VALIDATION.md`](docs/VALIDATION.md).

## Testing

```sh
make test
```

The checked-in SQL tests avoid live network calls. Use `ai_request_json` for
deterministic request-shape checks and run provider smoke tests manually with
real credentials or a local Ollama server. See
[`docs/SMOKE_TESTING.md`](docs/SMOKE_TESTING.md) for mock, Ollama, and remote
provider smoke commands.

To run a local mock-provider smoke test for `ai_complete`, `ai_usage()`, and the
HTTP log endpoint:

```sh
python3 test/smoke/mock_provider_smoke.py
```
