---
sidebar_position: 2
---

# SQL function reference

This page documents the public SQL surface registered by the `ai`
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
| `ai_try_complete(prompt[, model[, provider]])` | Scalar | Calls a completion model and returns `STRUCT(response, error)` for row-level failure capture. |
| `ai_complete_json(prompt[, model[, provider]])` | Scalar | Calls a completion model and validates the response as a JSON object or array. |
| `ai_complete_record(prompt, response_schema[, model[, provider]])` | Table | Calls a completion model and projects a JSON object into typed columns from a JSON Schema. |
| `ai_extract_record(text, response_schema[, model[, provider]])` | Scalar | Extracts one typed `STRUCT` per row from text using a JSON Schema. |
| `ai_completion_request_json(prompt[, model[, provider]])` | Scalar | Returns the completion request JSON without making a network call. |
| `ai_embed(text[, model[, provider]])` | Scalar | Calls an embedding model and returns `DOUBLE[]`. |
| `ai_embedding_request_json(text[, model[, provider]])` | Scalar | Returns the embedding request JSON without making a network call. |
| `ai_similarity(left_text, right_text[, model[, provider]])` | Scalar | Embeds two strings and returns cosine similarity. |
| `ai_rerank(query, candidate[, model[, provider]])` | Scalar | Uses a completion model to score candidate relevance from `0` to `1`. |
| `ai_summarize(text[, model[, provider]])` | Scalar | Summarizes text with a completion model. |
| `ai_sentiment(text[, model[, provider]])` | Scalar | Classifies text as positive, neutral, or negative. |
| `ai_fix_grammar(text[, model[, provider]])` | Scalar | Rewrites text with corrected grammar, spelling, and punctuation. |
| `ai_redact(text[, model[, provider]])` | Scalar | Masks direct personal data, credentials, secrets, and payment identifiers. |
| `ai_translate(text, target_language[, model[, provider]])` | Scalar | Translates text to the target language. |
| `ai_classify(text, labels[, model[, provider]])` | Scalar | Chooses one label from a comma-separated `VARCHAR` or `VARCHAR[]` label list. |
| `ai_classify_labels(text, labels[, model[, provider]])` | Scalar | Chooses zero or more labels from a comma-separated `VARCHAR` or `VARCHAR[]` label list. |
| `ai_extract(text, instruction[, model[, provider]])` | Scalar | Extracts requested information from text. |
| `ai_filter(text, predicate[, model[, provider]])` | Scalar | Evaluates a natural-language predicate and returns `BOOLEAN`. |
| `ai_agg(text, instruction[, model[, provider]])` | Aggregate | Runs one completion over grouped text values and an instruction. |
| `ai_summarize_agg(text[, model[, provider]])` | Aggregate | Summarizes grouped text values. |
| `ai_sql(question[, schema_context[, model[, provider]]])` | Scalar | Generates one read-only DuckDB `SELECT` statement. |
| `ai_query_data(question[, schema_context[, model[, provider]]])` | Table | Generates one read-only `SELECT` at bind time and executes it as a subquery. |
| `ai_schema_prompt([include_tables])` | Table | Returns deterministic local catalog context for prompting SQL models. |
| `ai_explain_sql(sql[, ...])` | Table | Explains one read-only DuckDB `SELECT` statement. |
| `ai_fix_sql(sql[, ...])` | Table | Rewrites a broken query as one corrected read-only DuckDB `SELECT`, or rewrites one line with `mode := 'line'`. |
| `ai_is_read_only_sql(sql[, check_binding])` | Scalar | Returns whether SQL is one parser-valid read-only `SELECT`; optionally also checks it binds against the catalog. |
| `ai_validate_read_only_sql(sql[, check_binding])` | Scalar | Returns normalized SQL or raises if it is not one read-only `SELECT`; optionally also checks it binds against the catalog. |
| `ai_count_tokens(text[, model[, provider]])` | Scalar | Returns a local approximate token count. |
| `ai_recommended_batch_size(input_tokens_per_row, max_output_tokens_per_row, token_limit_per_minute[, request_limit_per_minute[, safety_factor]])` | Scalar | Returns a conservative row batch size for rate-limited AI jobs. |
| `ai_provider_base_url(provider)` | Scalar | Returns the default base URL for a supported provider. |
| `ai_provider_protocol(provider)` | Scalar | Returns the internal provider protocol. |
| `ai_usage()` | Table | Returns recent per-database AI usage events. |
| `ai_clear_usage()` | Table | Clears the per-database usage event buffer. |
| `ai_clear_cache()` | Table | Clears per-database in-memory response and generated-SQL caches. |
| `ai_secrets()` | Table | Lists configured `duckdb_ai` secrets with credentials redacted. |
| `ai_model_prices()` | Table | Returns the built-in provider/model pricing catalog. |

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

#### `ai_try_complete(prompt[, model[, provider]])`

Description: Calls a configured completion provider and returns a
`STRUCT(response VARCHAR, error VARCHAR)`. Successful rows have `error = NULL`;
failed rows have `response = NULL` and the provider or validation error text.
Use this for batch enrichment when one bad row should be written to a rejected
rows table instead of failing the whole query.

Example:

```sql
WITH attempts AS (
    SELECT id, ai_try_complete(prompt, provider := 'openai') AS result
    FROM prompts
)
SELECT id, result.response
FROM attempts
WHERE result.error IS NULL;
```

Result: `STRUCT(response VARCHAR, error VARCHAR)`

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

#### `ai_extract_record(text, response_schema[, model[, provider]])`

Description: Scalar function that calls a completion provider for each input row
and returns a typed `STRUCT` projected from the top-level object properties in
`response_schema`. The schema must be constant so DuckDB can bind the return
type.

Example:

```sql
SELECT
    ticket_id,
    extracted.product_area,
    extracted.urgency_score
FROM (
    SELECT
        ticket_id,
        ai_extract_record(
            subject || ': ' || body,
            '{
              "type": "object",
              "properties": {
                "product_area": {"type": "string"},
                "urgency_score": {"type": "integer"}
              },
              "required": ["product_area", "urgency_score"]
            }'
        ) AS extracted
    FROM support_tickets
);
```

Result: one `STRUCT` value whose fields are derived from `response_schema`

#### `ai_completion_request_json(prompt[, model[, provider]])`

Description: Builds the provider completion request body and returns it without
making a network call. Use this for deterministic tests and provider debugging.

Example:

```sql
SELECT ai_completion_request_json(
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

#### `ai_rerank(query, candidate[, model[, provider]])`

Description: Uses a completion model to score how relevant `candidate` is for
`query`. The provider response must be a single numeric score from `0` to `1`.
Use it when you want LLM-based reranking over a short candidate set; use
`ai_similarity` for embedding-based semantic comparison at larger scale.

Example:

```sql
SELECT *
FROM documents
ORDER BY ai_rerank('analytics database', title || chr(10) || body) DESC
LIMIT 10;
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

Description: Convenience wrapper for `ai_classify(text, 'positive, neutral, negative')`.

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

Description: Classifies text into exactly one label from `labels`. `labels` can
be a comma-separated `VARCHAR` or a `VARCHAR[]`; use `VARCHAR[]` when labels may
contain commas.

Example:

```sql
SELECT ai_classify('invoice overdue', 'billing, support');

SELECT ai_classify('invoice overdue', ['billing, overdue', 'support']);
```

Result: `VARCHAR`

#### `ai_classify_labels(text, labels[, model[, provider]])`

Description: Classifies text into zero or more labels from `labels`. `labels`
can be a comma-separated `VARCHAR` or a `VARCHAR[]`; use `VARCHAR[]` when labels
may contain commas. The model must return a JSON array of label strings.

Example:

```sql
SELECT ai_classify_labels(
    'invoice overdue and app is slow',
    ['billing, overdue', 'performance', 'support']
);
```

Result: `VARCHAR[]`

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

Description: Convenience wrapper around `ai_agg` with the built-in summarization
instruction. It collects grouped input text and returns one summary per group.

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
context from `ai_schema_prompt()`. With `fix_attempts := N` (0 to 5, default 0),
the function verifies the generated SQL binds against the current catalog and,
when it does not, feeds the bind error back to the model for up to `N`
correction rounds before failing.

Example:

```sql
SELECT ai_sql('count orders by status');
```

Example with self-correction:

```sql
SELECT ai_sql('count orders by status', fix_attempts := 2);
```

Result: `VARCHAR` containing a read-only DuckDB `SELECT`

#### `ai_query_data(question[, schema_context[, model[, provider]]])`

Description: Table function that generates one read-only DuckDB `SELECT` at bind
time and executes it as a subquery. Successful generated SQL is cached in the
current DuckDB database instance for repeated binds with the same question,
schema context, and output-affecting options. Use `on_error := 'null'` to return
an empty error-shaped relation instead of failing the bind.

With `fix_attempts := N` (0 to 5, default 0), the function verifies the
generated SQL binds against the current catalog before returning it. When
binding fails — a hallucinated column, a wrong function name, a typo — the
DuckDB bind error is fed back to the model for up to `N` correction rounds.
Each correction is a regular model call and appears in `ai_usage()` alongside an
`ai_query_data_fix_attempt` event. Cached SQL is re-verified on cache hits when
`fix_attempts > 0`, so entries that stopped binding after a schema change are
repaired instead of failing.

Example:

```sql
SELECT *
FROM ai_query_data(
    'count orders by status',
    schema_context := (SELECT summary FROM ai_schema_prompt(include_tables := ['main.orders']))
);
```

Example with self-correction (useful in views, scheduled queries, and other
non-interactive contexts):

```sql
SELECT *
FROM ai_query_data(
    'revenue by pickup borough',
    include_tables := ['trips', 'zones'],
    fix_attempts := 2
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
corrected read-only DuckDB `SELECT` statement. With `mode := 'line'`, it rewrites
only the line identified by an error message and returns the detected line number
plus replacement line.

In query mode, pass `error :=` to include the failure message in the correction
prompt — the error text is usually what makes the fix reliable. Passing `error`
without an explicit `mode` selects line mode for backward compatibility, so
combine it with `mode := 'query'` for full-query rewrites. Query mode also
accepts `fix_attempts := N` (0 to 5, default 0) to verify the corrected SQL
binds against the current catalog and re-correct with the bind error for up to
`N` rounds.

Example:

```sql
SELECT sql
FROM ai_fix_sql('SEELECT status, count(*) FRUM orders GROUP BY status');
```

Example with the error message and bind verification:

```sql
SELECT sql
FROM ai_fix_sql(
    'SELECT statuss, count(*) FROM orders GROUP BY statuss',
    mode := 'query',
    error := 'Binder Error: Referenced column "statuss" not found',
    fix_attempts := 1
);
```

Result: one `sql VARCHAR` column

Example:

```sql
SELECT line_number, replacement_line
FROM ai_fix_sql(
    'SEELECT status FROM orders',
    mode := 'line',
    error := 'Parser Error: syntax error at or near "SEELECT" LINE 1'
);
```

Line-mode result: `line_number BIGINT`, `replacement_line VARCHAR`

## SQL validation functions

#### `ai_is_read_only_sql(sql[, check_binding])`

Description: Returns whether `sql` parses as exactly one read-only DuckDB
`SELECT` statement. With `check_binding` set to `true`, the statement must also
bind against the current catalog — hallucinated tables, columns, or functions
return `false` even when the SQL parses. The bind check runs in the current
session, so temporary tables are visible, and nothing is executed.

Example:

```sql
SELECT ai_is_read_only_sql('SELECT 42');
SELECT ai_is_read_only_sql('SELECT missing_column FROM my_table', true);
```

Result: `BOOLEAN`

#### `ai_validate_read_only_sql(sql[, check_binding])`

Description: Returns the input SQL when it is one read-only DuckDB `SELECT`;
otherwise raises an error. With `check_binding` set to `true`, the statement
must also bind against the current catalog.

Example:

```sql
SELECT ai_validate_read_only_sql('SELECT count(*) FROM my_table');
SELECT ai_validate_read_only_sql('SELECT count(*) FROM my_table', true);
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

#### `ai_recommended_batch_size(input_tokens_per_row, max_output_tokens_per_row, token_limit_per_minute[, request_limit_per_minute[, safety_factor]])`

Description: Returns a conservative number of rows to process per batch or
minute for a rate-limited AI job. `input_tokens_per_row` can come from
`avg(ai_count_tokens(prompt))`, `max_output_tokens_per_row` should match the
planned `max_tokens`, and `safety_factor` defaults to `0.8`.

Example:

```sql
SELECT ai_recommended_batch_size(100, 200, 200000, 500);
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

Description: Returns recent completion, embedding, and local usage events for
the current DuckDB database instance. The buffer keeps the latest 1,024 events.

Example:

```sql
SELECT *
FROM ai_usage()
ORDER BY event_id DESC;
```

Result columns: `event_id`, `created_at`, `event`, `function_name`, `query_id`,
`provider`, `protocol`, `model`, character counts, token counts,
cached-token counts, elapsed time, retry count, HTTP status, cache hit flag,
status, error, and estimated cost.

#### `ai_clear_usage()`

Description: Clears the current DuckDB database instance's usage event buffer
and returns one confirmation row.

Example:

```sql
SELECT *
FROM ai_clear_usage();
```

Result: one `cleared BOOLEAN` column

#### `ai_clear_cache()`

Description: Clears the current DuckDB database instance's opt-in in-memory
response cache and `ai_query_data()` generated-SQL cache, then returns one
confirmation row.

Example:

```sql
SELECT *
FROM ai_clear_cache();
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

#### `ai_model_prices()`

Description: Returns the small built-in provider/model pricing catalog used by
opt-in cost estimation.

Example:

```sql
SELECT provider, model, operation, input_token_price_per_million
FROM ai_model_prices();
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
| `connect_timeout_seconds` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | HTTP connection timeout from 1 to 31536000 seconds. It must be less than or equal to the total timeout when a provider call runs. |
| `retry_count` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Retry count from 0 to 10 for curl failures and retryable HTTP status codes. Retries honor `Retry-After` on 429/5xx responses when present. |
| `retry_backoff_ms` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Base retry backoff from 0 to 60000 milliseconds; exponential jitter is added per retry. |
| `max_concurrent_requests` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Per-database request concurrency cap from 0 to 64. |
| `min_request_interval_ms` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Per-database minimum interval between provider request starts. |
| `token_limit_per_minute` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Per-database estimated token cap per rolling minute. `0` disables the token cap. |
| `cache` | `BOOLEAN` | Completion, embedding, SQL assistant, aggregate | Enables the current database instance's in-memory response cache for successful provider responses. |
| `cache_ttl_seconds` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Optional response-cache expiration from 0 to 31536000 seconds. `0` means entries do not expire by age. The `DUCKDB_AI_CACHE_TTL_SECONDS` environment variable sets the default. |
| `cache_max_entries` | `BIGINT` | Completion, embedding, SQL assistant, aggregate | Maximum in-memory response-cache entries from 0 to 1000000. `0` disables response-cache storage. The `DUCKDB_AI_CACHE_MAX_ENTRIES` environment variable sets the default. |
| `prompt_cache` | `BOOLEAN` | Completion, task wrappers, and SQL assistant | Emits provider-side prompt-cache controls where supported. OpenAI requests include a stable `prompt_cache_key`; Anthropic requests attach ephemeral cache control to the system prompt. The `DUCKDB_AI_PROMPT_CACHE` environment variable enables this by default. |
| `allowed_hosts` | `VARCHAR` | Completion, embedding, SQL assistant, aggregate | Comma-separated provider/logging host allowlist. Entries may be hostnames, `host:port`, full URLs, `*.example.com`, or `*`. |
| `on_error` | `VARCHAR` | Completion, embedding, SQL assistant, aggregate | Error handling mode: `fail`, `null`, or `capture`. `capture` is used by `ai_try_complete`; scalar/table functions that cannot return an error field use `NULL` behavior. |
| `fail_on_error` | `BOOLEAN` | Completion, embedding, SQL assistant, aggregate | Compatibility alias: `true` maps to `on_error := 'fail'`, `false` maps to `on_error := 'null'`. |
| `response_format` | `VARCHAR` | Completion and SQL assistant | `text`, `json_object`, or `json_schema`. |
| `response_schema`, `json_schema` | `VARCHAR` | Completion and SQL assistant | JSON Schema object for structured output hints and validation. |
| `input_token_price_per_million` | `DOUBLE` | Completion, embedding, SQL assistant, aggregate | Manual input-token price for cost estimation. |
| `output_token_price_per_million` | `DOUBLE` | Completion, SQL assistant, aggregate | Manual output-token price for cost estimation. |
| `use_builtin_model_prices` | `BOOLEAN` | Completion, embedding, SQL assistant, aggregate | Enables lookup from `ai_model_prices()` when manual prices are not supplied. |
| `log_format` | `VARCHAR` | Completion, embedding, SQL assistant, aggregate | `generic`, `json`, `generic_json`, `otlp`, or `otlp_json`. |
| `log_tags` | `VARCHAR` | Completion, embedding, SQL assistant, aggregate | Comma-separated tags copied into usage log payloads. |
| `log_sample_rate` | `DOUBLE` | Completion, embedding, SQL assistant, aggregate | Stable sampling rate from 0 to 1. |
| `log_include_text` | `BOOLEAN` | `ai_complete_record`; all families through `duckdb_ai_log_include_text` or `DUCKDB_AI_LOG_INCLUDE_TEXT` | Include prompt/output text in outbound usage logs. Defaults to false. |
| `log_strict` | `BOOLEAN` | `ai_complete_record`; all families through `duckdb_ai_log_strict` or `DUCKDB_AI_LOG_STRICT` | Post outbound usage logs synchronously and fail the SQL query when logging fails. Without strict logging, outbound logs are queued asynchronously on a best-effort basis. |

See [Runtime behavior](runtime-behavior.md) for the execution semantics behind
volatility, per-database state, concurrency, cancellation, retries, response
caching, and egress allowlisting.

SQL assistant table functions also accept:

| Option | Type | Description |
| --- | --- | --- |
| `schema_context`, `schema` | `VARCHAR` | Prompt context to use instead of generated local catalog context. |
| `include_tables` | `VARCHAR[]` | Limit generated local catalog context to matching tables. |
| `exclude_tables` | `VARCHAR[]` | Remove matching tables from generated local catalog context. |
| `sample_rows` | `BIGINT` | Include up to 100 sample rows per local table. |
| `mode` | `VARCHAR` | `ai_fix_sql` only. `query` or `full` rewrites the full query; `line` rewrites one error line. |
| `error` | `VARCHAR` | `ai_fix_sql` only. Error text included in the correction prompt; in line mode it also identifies the target line. Without an explicit `mode`, passing `error` selects line mode. |
| `fix_attempts` | `BIGINT` | `ai_sql`, `ai_query_data`, and `ai_fix_sql` query mode. Number of bind-verified self-correction rounds from 0 to 5. Default 0 keeps single-shot behavior. |

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
| `anthropic` | `claude` | Yes | No | `claude-haiku-4-5` |
| `gemini` | `gcp`, `google`, `google_gemini` | Yes | Yes | `gemini-3.5-flash` |
| `mistral` | none | Yes | Yes | `mistral-small-latest` |
| `zai` | `zhipu` | Yes | Yes | `glm-4.7-flash` |
| `deepseek` | none | Yes | No | `deepseek-v4-flash` |
| `openrouter` | none | Yes | Yes | `openai/gpt-4o-mini` |
| `databricks` | `mosaic`, `mosaic_ai`, `databricks_ai` | Yes | No | `databricks-llama-4-maverick` |
| `snowflake` | none | Yes | No | `snowflake-llama-3.3-70b` |
| `openai_privacy_filter` | `privacy_filter`, `pii_filter`, `opf` | Redaction only | No | `openai/privacy-filter` |
| `openai_compatible` | `local`, `openai-compatible`, `local_openai`, `local-models`, `local_models` | Yes | Yes | `gpt-4o-mini` |
| `llamacpp` | `llama.cpp`, `llama-cpp`, `llama_cpp`, `llama-server`, `llama_server` | Yes | Yes | `default` (llama-server answers with its loaded model) |

Session defaults can be configured with DuckDB settings:

```sql
SET duckdb_ai_provider = 'openai';
SET duckdb_ai_model = 'gpt-4o-mini';
SET duckdb_ai_embedding_model = 'text-embedding-3-small';
SET duckdb_ai_base_url = 'https://api.openai.com/v1';
SET duckdb_ai_timeout_seconds = 120;
SET duckdb_ai_allowed_hosts = 'api.openai.com,collector.example';
SET duckdb_ai_cache = true;
```

`duckdb_ai_model` is the global model fallback. Family-specific model settings
override it for their function groups, while per-call `model := ...` still takes
highest precedence:

| Setting | Applies to |
| --- | --- |
| `duckdb_ai_completion_model` | `ai_complete`, `ai_complete_json`, `ai_complete_record`, `ai_completion_request_json`, `ai_rerank` |
| `duckdb_ai_task_model` | `ai_summarize`, `ai_sentiment`, `ai_fix_grammar`, `ai_redact`, `ai_translate`, `ai_classify`, `ai_classify_labels`, `ai_extract`, `ai_filter` |
| `duckdb_ai_aggregate_model` | `ai_agg`, `ai_summarize_agg` |
| `duckdb_ai_sql_assistant_model` | `ai_sql`, `ai_query_data`, `ai_explain_sql`, `ai_fix_sql` |
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
