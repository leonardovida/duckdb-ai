---
sidebar_position: 2
---

# SQL function reference

This page documents the public SQL surface registered by the `duckdb_ai`
extension. The layout follows the DuckDB function reference style: a compact
overview table first, then each function with description, example, and result
shape.

Table functions must be called from `FROM` or with `CALL`, for example:

```sql
SELECT *
FROM ai_usage();
```

## Function overview

| Function | Type | Description |
| --- | --- | --- |
| `ai_complete(prompt[, model[, provider]])` | Scalar | Calls a completion model and returns text. |
| `ai_complete_json(prompt[, model[, provider]])` | Scalar | Calls a completion model and validates the response as a JSON object or array. |
| `ai_complete_record(prompt, response_schema[, model[, provider]])` | Table | Calls a completion model and projects a JSON object into typed columns from a JSON Schema. |
| `ai_request_json(prompt[, model[, provider]])` | Scalar | Returns the completion request JSON without making a network call. |
| `ai_embed(text[, model[, provider]])` | Scalar | Calls an embedding model and returns `DOUBLE[]`. |
| `ai_embedding_request_json(text[, model[, provider]])` | Scalar | Returns the embedding request JSON without making a network call. |
| `ai_similarity(left_text, right_text[, model[, provider]])` | Scalar | Embeds two strings and returns cosine similarity. |
| `ai_summarize(text[, model[, provider]])` | Scalar | Summarizes text with a completion model. |
| `ai_sentiment(text[, model[, provider]])` | Scalar | Classifies text as positive, neutral, or negative. |
| `ai_fix_grammar(text[, model[, provider]])` | Scalar | Rewrites text with corrected grammar, spelling, and punctuation. |
| `ai_redact(text[, model[, provider]])` | Scalar | Masks direct personal data, credentials, secrets, and payment identifiers. |
| `ai_translate(text, target_language[, model[, provider]])` | Scalar | Translates text to the target language. |
| `ai_classify(text, labels[, model[, provider]])` | Scalar | Chooses one label from a comma-separated label list. |
| `ai_extract(text, instruction[, model[, provider]])` | Scalar | Extracts requested information from text. |
| `ai_filter(text, predicate[, model[, provider]])` | Scalar | Evaluates a natural-language predicate and returns `BOOLEAN`. |
| `ai_agg(text, instruction[, model[, provider]])` | Aggregate | Runs one completion over grouped text values and an instruction. |
| `ai_summarize_agg(text[, model[, provider]])` | Aggregate | Summarizes grouped text values. |
| `ai_sql(question[, schema_context[, model[, provider]]])` | Scalar | Generates one read-only DuckDB `SELECT` statement. |
| `ai_query_data(question[, schema_context[, model[, provider]]])` | Table | Generates one read-only `SELECT` at bind time and executes it as a subquery. |
| `ai_schema_prompt([include_tables])` | Table | Returns deterministic local catalog context for prompting SQL models. |
| `ai_explain_sql(sql[, ...])` | Table | Explains one read-only DuckDB `SELECT` statement. |
| `ai_fix_sql(sql[, ...])` | Table | Rewrites a broken query as one corrected read-only DuckDB `SELECT`. |
| `ai_fix_sql_line(sql[, error][, ...])` | Table | Rewrites only the line identified by an error message. |
| `ai_is_read_only_sql(sql)` | Scalar | Returns whether SQL is one parser-valid read-only `SELECT`. |
| `ai_validate_read_only_sql(sql)` | Scalar | Returns normalized SQL or raises if it is not one read-only `SELECT`. |
| `ai_count_tokens(text[, model[, provider]])` | Scalar | Returns a local approximate token count. |
| `ai_provider_base_url(provider)` | Scalar | Returns the default base URL for a supported provider. |
| `ai_provider_protocol(provider)` | Scalar | Returns the internal provider protocol. |
| `ai_usage()` | Table | Returns recent in-process AI usage events. |
| `ai_clear_usage()` | Table | Clears the in-process usage event buffer. |
| `ai_secrets()` | Table | Lists configured `duckdb_ai` secrets with credentials redacted. |
| `ai_models()` | Table | Returns the built-in provider/model pricing catalog. |

## Completion functions

#### `ai_complete(prompt[, model[, provider]])`

Description: Calls a configured completion provider and returns the completion
text as `VARCHAR`.

Example:

```sql
SELECT ai_complete(
    'Write one sentence about DuckDB.',
    provider := 'openai',
    model := 'gpt-4o-mini'
);
```

Result: `VARCHAR`

#### `ai_complete_json(prompt[, model[, provider]])`

Description: Calls a completion provider, asks for JSON-only output, and
validates that the returned text is a top-level JSON object or array. If
`response_schema` or `json_schema` is supplied, the output is also validated
against the supported JSON Schema subset.

Example:

```sql
SELECT ai_complete_json(
    'Extract a compact company profile for DuckDB.',
    provider := 'openai',
    response_schema := '{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}'
);
```

Result: `VARCHAR` containing valid JSON

#### `ai_complete_record(prompt, response_schema[, model[, provider]])`

Description: Table function that calls a completion provider, validates the JSON
response against `response_schema`, and projects top-level schema properties as
typed DuckDB columns.

Example:

```sql
SELECT *
FROM ai_complete_record(
    'Extract a compact company profile for DuckDB.',
    '{"type":"object","properties":{"name":{"type":"string"},"score":{"type":"number"}},"required":["name"]}',
    provider := 'openai'
);
```

Result: one row with columns derived from `response_schema`

#### `ai_request_json(prompt[, model[, provider]])`

Description: Builds the provider completion request body and returns it without
making a network call. Use this for deterministic tests and provider debugging.

Example:

```sql
SELECT ai_request_json(
    'Summarize this text.',
    provider := 'openai',
    model := 'gpt-4o-mini',
    temperature := 0.2
);
```

Result: `VARCHAR` containing provider request JSON

## Embedding functions

#### `ai_embed(text[, model[, provider]])`

Description: Calls an embedding provider and returns a DuckDB list of doubles.

Example:

```sql
SELECT ai_embed(
    'DuckDB is an analytical database.',
    provider := 'openai',
    model := 'text-embedding-3-small'
);
```

Result: `DOUBLE[]`

#### `ai_embedding_request_json(text[, model[, provider]])`

Description: Builds the provider embedding request body and returns it without
making a network call.

Example:

```sql
SELECT ai_embedding_request_json(
    'DuckDB vector smoke',
    provider := 'openai',
    model := 'text-embedding-3-small'
);
```

Result: `VARCHAR` containing provider request JSON

#### `ai_similarity(left_text, right_text[, model[, provider]])`

Description: Embeds both input strings with the same provider/model and returns
cosine similarity.

Example:

```sql
SELECT ai_similarity(
    'DuckDB analytics',
    'analytical SQL database',
    provider := 'openai',
    model := 'text-embedding-3-small'
);
```

Result: `DOUBLE`

## Task wrappers

#### `ai_summarize(text[, model[, provider]])`

Description: Summarizes text and returns only the summary.

Example:

```sql
SELECT ai_summarize('DuckDB is an analytical database built for fast local queries.');
```

Result: `VARCHAR`

#### `ai_sentiment(text[, model[, provider]])`

Description: Classifies the sentiment of text as positive, neutral, or negative.

Example:

```sql
SELECT ai_sentiment('The import finished quickly and the query is fast.');
```

Result: `VARCHAR`

#### `ai_fix_grammar(text[, model[, provider]])`

Description: Fixes grammar, spelling, and punctuation while preserving meaning.

Example:

```sql
SELECT ai_fix_grammar('duckdb are fast');
```

Result: `VARCHAR`

#### `ai_redact(text[, model[, provider]])`

Description: Masks direct personal data, credentials, secrets, and payment
identifiers. With provider `openai_privacy_filter`, sends the raw input text to
a local or cloud-hosted OpenAI Privacy Filter service instead of wrapping it in a
chat prompt.

Example:

```sql
SELECT ai_redact('email alice@example.com');

SELECT ai_redact(
    'email alice@example.com token fake-token',
    provider := 'openai_privacy_filter',
    base_url := 'http://localhost:8080'
);
```

Result: `VARCHAR`

#### `ai_translate(text, target_language[, model[, provider]])`

Description: Translates text to `target_language` while preserving meaning and
formatting.

Example:

```sql
SELECT ai_translate('hello', 'Dutch');
```

Result: `VARCHAR`

#### `ai_classify(text, labels[, model[, provider]])`

Description: Classifies text into exactly one label from `labels`.

Example:

```sql
SELECT ai_classify('invoice overdue', 'billing, support');
```

Result: `VARCHAR`

#### `ai_extract(text, instruction[, model[, provider]])`

Description: Extracts requested information from text. When the instruction asks
for structured data, the function prompts for concise JSON.

Example:

```sql
SELECT ai_extract('name: DuckDB', 'name');
```

Result: `VARCHAR`

#### `ai_filter(text, predicate[, model[, provider]])`

Description: Evaluates whether `text` matches a natural-language predicate. The
model output must parse as true or false.

Example:

```sql
SELECT ai_filter('invoice overdue', 'is about billing');
```

Result: `BOOLEAN`

## Aggregate functions

#### `ai_agg(text, instruction[, model[, provider]])`

Description: Collects grouped input text up to `max_context_chars`, then calls
one completion model with `instruction`.

Example:

```sql
SELECT ai_agg(message, 'List the top three recurring themes')
FROM customer_feedback;
```

Result: `VARCHAR`

#### `ai_summarize_agg(text[, model[, provider]])`

Description: Collects grouped input text and returns one summary per group.

Example:

```sql
SELECT customer_id, ai_summarize_agg(note ORDER BY created_at)
FROM support_notes
GROUP BY customer_id;
```

Result: `VARCHAR`

## SQL assistant functions

#### `ai_sql(question[, schema_context[, model[, provider]]])`

Description: Calls a completion model to generate one DuckDB `SELECT` statement,
strips common markdown code fences, and rejects output that is not one read-only
`SELECT`. When `schema_context` is omitted, the function builds local catalog
context from `ai_schema_prompt()`.

Example:

```sql
SELECT ai_sql('count orders by status');
```

Result: `VARCHAR` containing a read-only DuckDB `SELECT`

#### `ai_query_data(question[, schema_context[, model[, provider]]])`

Description: Table function that generates one read-only DuckDB `SELECT` at bind
time and executes it as a subquery. Use `fail_on_error := false` to return an
empty error-shaped relation instead of failing the bind.

Example:

```sql
SELECT *
FROM ai_query_data(
    'count orders by status',
    schema_context := (SELECT summary FROM ai_schema_prompt(include_tables := ['main.orders']))
);
```

Result: the result columns of the generated query

#### `ai_schema_prompt([include_tables])`

Description: Table function that returns one `summary` row with deterministic
DuckDB catalog context. `include_tables` and `exclude_tables` accept table,
`schema.table`, or `catalog.schema.table` names. `sample_rows` includes bounded
sample rows for local DuckDB tables during execution.

Example:

```sql
SELECT summary
FROM ai_schema_prompt(
    include_tables := ['main.orders'],
    exclude_tables := ['main.internal_audit'],
    sample_rows := 3
);
```

Result: one `summary VARCHAR` column

#### `ai_explain_sql(sql[, ...])`

Description: Table function that explains one read-only DuckDB `SELECT`
statement using optional schema context.

Example:

```sql
SELECT explanation
FROM ai_explain_sql(
    'SELECT status, count(*) FROM orders GROUP BY status',
    include_tables := ['main.orders']
);
```

Result: one `explanation VARCHAR` column

#### `ai_fix_sql(sql[, ...])`

Description: Table function that asks the model to rewrite a broken query as one
corrected read-only DuckDB `SELECT` statement.

Example:

```sql
SELECT sql
FROM ai_fix_sql('SEELECT status, count(*) FRUM orders GROUP BY status');
```

Result: one `sql VARCHAR` column

#### `ai_fix_sql_line(sql[, error][, ...])`

Description: Table function that asks the model to rewrite only the line
identified by an error message. The result includes the detected line number and
replacement line.

Example:

```sql
SELECT line_number, replacement_line
FROM ai_fix_sql_line(
    'SEELECT status FROM orders',
    error := 'Parser Error: syntax error at or near "SEELECT" LINE 1'
);
```

Result: `line_number BIGINT`, `replacement_line VARCHAR`

## SQL validation functions

#### `ai_is_read_only_sql(sql)`

Description: Returns whether `sql` parses as exactly one read-only DuckDB
`SELECT` statement.

Example:

```sql
SELECT ai_is_read_only_sql('SELECT 42');
```

Result: `BOOLEAN`

#### `ai_validate_read_only_sql(sql)`

Description: Returns the input SQL when it is one read-only DuckDB `SELECT`;
otherwise raises an error.

Example:

```sql
SELECT ai_validate_read_only_sql('SELECT count(*) FROM my_table');
```

Result: `VARCHAR`

#### `ai_count_tokens(text[, model[, provider]])`

Description: Returns a local approximate token count. This function does not
call a provider.

Example:

```sql
SELECT ai_count_tokens('DuckDB local analytics');
```

Result: `BIGINT`

## Provider metadata functions

#### `ai_provider_base_url(provider)`

Description: Returns the default base URL for a supported provider after alias
normalization.

Example:

```sql
SELECT ai_provider_base_url('ollama');
```

Result: `VARCHAR`

#### `ai_provider_protocol(provider)`

Description: Returns the internal request protocol used for provider request and
response shaping.

Example:

```sql
SELECT ai_provider_protocol('anthropic');
```

Result: `VARCHAR`

## Usage and catalog table functions

#### `ai_usage()`

Description: Returns recent in-process completion, embedding, and local usage
events. The buffer keeps the latest 1,024 events.

Example:

```sql
SELECT *
FROM ai_usage()
ORDER BY event_id DESC;
```

Result columns: `event_id`, `created_at`, `event`, `provider`, `protocol`,
`model`, character counts, token counts, elapsed time, HTTP status, and
estimated cost.

#### `ai_clear_usage()`

Description: Clears the in-process usage event buffer and returns one
confirmation row.

Example:

```sql
SELECT *
FROM ai_clear_usage();
```

Result: one `cleared BOOLEAN` column

#### `ai_secrets()`

Description: Lists configured `duckdb_ai` secrets with credential values
redacted.

Example:

```sql
SELECT name, provider, model, base_url, has_api_key
FROM ai_secrets();
```

Result columns: `name`, `provider`, `model`, `base_url`, `scope`,
`has_api_key`

#### `ai_models()`

Description: Returns the small built-in provider/model pricing catalog used by
opt-in cost estimation.

Example:

```sql
SELECT provider, model, operation, input_token_price_per_million
FROM ai_models();
```

Result columns: `provider`, `model`, `operation`,
`input_token_price_per_million`, `output_token_price_per_million`,
`source_url`, `source_note`, `last_reviewed`

## Named options

Completion functions accept constant named options unless noted otherwise.
Provider credentials are resolved from environment variables or DuckDB secrets,
not from direct API key arguments.

| Option | Type | Applies to | Description |
| --- | --- | --- | --- |
| `model` | `VARCHAR` | Completion, embedding, SQL assistant, aggregate | Provider model name. |
| `provider` | `VARCHAR` | Completion, embedding, SQL assistant, aggregate | Provider name or alias. |
| `profile`, `secret`, `secret_name` | `VARCHAR` | Completion, embedding, SQL assistant, aggregate | DuckDB secret name or profile. |
| `temperature` | `DOUBLE` | Completion, SQL assistant, aggregate | Sampling temperature between 0 and 2. |
| `system_prompt` | `VARCHAR` | Completion, SQL assistant, aggregate | Optional system message for providers that support chat-style payloads. |
| `max_tokens` | `BIGINT` | Completion, SQL assistant, aggregate | Maximum provider output tokens. Must be greater than 0. |
| `base_url` | `VARCHAR` | Completion, embedding, SQL assistant, aggregate | Provider or gateway base URL override. |
| `timeout_seconds` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | HTTP timeout. Must be greater than 0. |
| `retry_count` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Retry count from 0 to 10 for curl failures and retryable HTTP status codes. |
| `retry_backoff_ms` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Base retry backoff from 0 to 60000 milliseconds. |
| `max_concurrent_requests` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Process-local request concurrency cap from 0 to 1024. |
| `min_request_interval_ms` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Process-local minimum interval between provider request starts. |
| `fail_on_error` | `BOOLEAN` | Completion, embedding, SQL assistant, aggregate | If false, supported functions return `NULL` or an empty result instead of raising provider errors. |
| `response_format` | `VARCHAR` | Completion and SQL assistant | `text`, `json_object`, or `json_schema`. |
| `response_schema`, `json_schema` | `VARCHAR` | Completion and SQL assistant | JSON Schema object for structured output hints and validation. |
| `input_token_price_per_million` | `DOUBLE` | Completion, embedding, SQL assistant, aggregate | Manual input-token price for cost estimation. |
| `output_token_price_per_million` | `DOUBLE` | Completion, SQL assistant, aggregate | Manual output-token price for cost estimation. |
| `use_builtin_model_prices` | `BOOLEAN` | Completion, embedding, SQL assistant, aggregate | Enables lookup from `ai_models()` when manual prices are not supplied. |
| `log_format` | `VARCHAR` | Completion, embedding, SQL assistant, aggregate | `generic_json` or `otlp_json`. |
| `log_tags` | `VARCHAR` | Completion, embedding, SQL assistant, aggregate | Comma-separated tags copied into usage log payloads. |
| `log_sample_rate` | `DOUBLE` | Completion, embedding, SQL assistant, aggregate | Stable sampling rate from 0 to 1. |
| `log_include_text` | `BOOLEAN` | `ai_complete_record`; all families through `duckdb_ai_log_include_text` or `DUCKDB_AI_LOG_INCLUDE_TEXT` | Include prompt/output text in outbound usage logs. Defaults to false. |
| `log_strict` | `BOOLEAN` | `ai_complete_record`; all families through `duckdb_ai_log_strict` or `DUCKDB_AI_LOG_STRICT` | Fail the SQL query when outbound usage logging fails. |

SQL assistant table functions also accept:

| Option | Type | Description |
| --- | --- | --- |
| `schema_context`, `schema` | `VARCHAR` | Prompt context to use instead of generated local catalog context. |
| `include_tables` | `VARCHAR[]` | Limit generated local catalog context to matching tables. |
| `exclude_tables` | `VARCHAR[]` | Remove matching tables from generated local catalog context. |
| `sample_rows` | `BIGINT` | Include up to 100 sample rows per local table. |
| `error` | `VARCHAR` | `ai_fix_sql_line` only. Error text used to identify the target line. |

Aggregate functions also accept:

| Option | Type | Description |
| --- | --- | --- |
| `instruction`, `task` | `VARCHAR` | Constant instruction for `ai_agg`. |
| `separator` | `VARCHAR` | Separator inserted between grouped input values. |
| `max_context_chars` | `BIGINT` | Maximum grouped text characters sent to the model. |

## Provider settings and secrets

Supported provider names and aliases are:

| Provider | Aliases | Completion support | Embedding support | Default completion model |
| --- | --- | --- | --- | --- |
| `ollama` | none | Yes | Yes | `llama3.2` |
| `openai` | none | Yes | Yes | `gpt-4o-mini` |
| `azure` | `azure_openai`, `azure-openai` | Yes | Yes | `gpt-4o` |
| `claude` | `anthropic` | Yes | No | `claude-3-5-haiku-latest` |
| `gemini` | `gcp`, `google`, `google_gemini` | Yes | Yes | `gemini-2.5-flash` |
| `mistral` | none | Yes | Yes | `mistral-small-latest` |
| `zai` | `zhipu` | Yes | Yes | `glm-4-flash` |
| `deepseek` | none | Yes | No | `deepseek-chat` |
| `openrouter` | none | Yes | Yes | `openai/gpt-4o-mini` |
| `databricks` | `mosaic`, `mosaic_ai`, `databricks_ai` | Yes | No | `databricks-llama-4-maverick` |
| `snowflake` | none | Yes | No | `snowflake-llama-3.3-70b` |
| `openai_privacy_filter` | `privacy_filter`, `pii_filter`, `opf` | Redaction only | No | `openai/privacy-filter` |
| `openai_compatible` | `local`, `openai-compatible`, `local_openai`, `local-models`, `local_models` | Yes | Yes | `gpt-4o-mini` |

Session defaults can be configured with DuckDB settings:

```sql
SET duckdb_ai_provider = 'openai';
SET duckdb_ai_model = 'gpt-4o-mini';
SET duckdb_ai_embedding_model = 'text-embedding-3-small';
SET duckdb_ai_base_url = 'https://api.openai.com/v1';
SET duckdb_ai_timeout_seconds = 120;
```

`duckdb_ai_model` is the global model fallback. Family-specific model settings
override it for their function groups, while per-call `model := ...` still takes
highest precedence:

| Setting | Applies to |
| --- | --- |
| `duckdb_ai_completion_model` | `ai_complete`, `ai_complete_json`, `ai_complete_record`, `ai_request_json` |
| `duckdb_ai_task_model` | `ai_summarize`, `ai_sentiment`, `ai_fix_grammar`, `ai_redact`, `ai_translate`, `ai_classify`, `ai_extract`, `ai_filter` |
| `duckdb_ai_aggregate_model` | `ai_agg`, `ai_summarize_agg` |
| `duckdb_ai_sql_model` | `ai_sql`, `ai_query_data`, `ai_explain_sql`, `ai_fix_sql`, `ai_fix_sql_line` |
| `duckdb_ai_embedding_model` | `ai_embed`, `ai_embedding_request_json`, `ai_similarity` |

Model resolution order is:

1. Per-call `model := ...`
2. Matching function-family setting
3. `duckdb_ai_model`
4. Provider default model

Credentials should use environment variables or DuckDB secrets:

```sql
CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    API_KEY '...',
    MODEL 'gpt-4o-mini'
);

SELECT ai_complete('hello', secret := 'openai_ai');
```
