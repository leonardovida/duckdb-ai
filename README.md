<p align="center">
  <img src="docs/assets/duckdb-ai-logo.svg" alt="duckdb-ai logo" width="520">
</p>

# duckdb-ai

[![CI](https://github.com/leonardovida/duckdb-ai/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/leonardovida/duckdb-ai/actions/workflows/MainDistributionPipeline.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Run language models on the data you already have in DuckDB — summarize,
classify, extract, embed, redact, and generate SQL, all from plain SQL
functions. No pipeline to a separate application, no data leaving your
machine unless you choose a hosted provider.

```sql
LOAD ai;

SELECT ai_summarize(
    'DuckDB is an analytical database built for fast local queries.',
    provider := 'ollama',
    model := 'gemma4:e4b'
);
```

One extension covers both ends of the spectrum:

- **Working on your laptop?** Point it at [Ollama](https://ollama.com) or any
  OpenAI-compatible local server and enrich tables with a free local model —
  nothing ever leaves your machine.
- **Running this for a team or company?** Route calls through your own gateway
  (Azure OpenAI, Databricks, Snowflake Cortex, vLLM, LiteLLM), keep credentials
  in DuckDB secrets or environment variables, restrict network egress to an
  allowlist, track per-call tokens and estimated cost in `ai_usage()`, and ship
  privacy-minimized usage logs to your own collector.

## What You Can Do

- Summarize, translate, redact, classify, and extract text from SQL queries.
- Generate valid JSON, typed DuckDB columns, or per-row `STRUCT` values from
  model output, validated against a JSON Schema.
- Create embeddings (batched automatically per chunk), compare text with cosine
  similarity, and rerank candidates.
- Ask questions about local tables and generate read-only DuckDB `SELECT`
  statements — generated SQL is parser-validated before it runs.
- Route model calls across local Ollama, hosted providers, and
  OpenAI-compatible gateways, with per-call, per-session, or secret-based
  configuration.
- Store credentials in DuckDB secrets instead of passing API keys through SQL
  function arguments.
- Control throughput with concurrency caps, request pacing, token-per-minute
  budgets, retries with backoff, and opt-in response and prompt caching.
- Inspect local usage events — including failures, retries, cache hits, and
  estimated cost — and optionally send privacy-minimized usage logs to your own
  collector.

## Quick Start

The `ai` extension is published as a [DuckDB community extension](https://duckdb.org/community_extensions/extensions/ai).
Install and load it from any DuckDB client:

```sql
INSTALL ai FROM community;
LOAD ai;
```

To build from source instead (requires a C++ toolchain, CMake, and ninja):

```sh
GEN=ninja make release
./build/release/duckdb
```

Confirm that the extension loaded:

```sql
SELECT ai_provider_protocol('openai');
```

For a local model with [Ollama](https://ollama.com/download), install Ollama
first. Then start the local Ollama server and download the Gemma model used by
the example:

```sh
ollama serve
ollama pull gemma4:e4b
```

```sql
LOAD ai;

SET duckdb_ai_provider = 'ollama';
SET duckdb_ai_model = 'gemma4:e4b';

SELECT ai_complete('Write one sentence about DuckDB.');
```

For OpenAI, store the API key in a DuckDB secret:

```sql
LOAD ai;

CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    API_KEY '...',
    MODEL 'gpt-4o-mini'
);

SELECT ai_complete(
    'Write one sentence about DuckDB.',
    secret := 'openai_ai'
);
```

`API_KEY` values stored in `TYPE duckdb_ai` secrets are redacted when secrets are
listed.

## Everyday Examples

The examples below use ordinary table columns. For pasteable end-to-end
walkthroughs, use the docs cookbooks:

- [Create the sample support tickets table](docs/cookbooks/support-ticket-data.md)
- [Enrich support tickets with AI text functions](docs/cookbooks/support-ticket-enrichment.md)
- [Compare support tickets with embeddings](docs/cookbooks/support-ticket-similarity.md)
- [Store embeddings in Lance for semantic search](docs/cookbooks/lance-semantic-search.md)
- [Extract typed records from model output](docs/cookbooks/structured-triage-records.md)
- [Generate read-only SQL over local tables](docs/cookbooks/sql-assistant.md)

Summarize support notes by customer:

```sql
SELECT
    customer_id,
    ai_summarize_agg(subject || ': ' || body ORDER BY created_at) AS summary
FROM support_tickets
GROUP BY customer_id;
```

Classify tickets using their subject and body:

```sql
SELECT
    ticket_id,
    ai_classify(
        subject || chr(10) || body,
        'billing, performance, integration, documentation, other'
    ) AS category
FROM support_tickets;
```

Filter rows with a natural-language predicate over multiple text columns:

```sql
SELECT *
FROM support_tickets
WHERE ai_filter(
    subject || chr(10) || body || chr(10) || internal_note,
    'mentions urgent production impact or needs engineering follow-up'
);
```

Extract compact JSON from a body column:

```sql
SELECT
    ticket_id,
    ai_extract(
        body,
        'Return compact JSON with product_area, customer_request, and urgency'
    ) AS extracted_json
FROM support_tickets;
```

Redact internal notes before sharing them:

```sql
-- Run OpenAI Privacy Filter locally, or point BASE_URL at a cloud-hosted wrapper.
SET duckdb_ai_provider = 'openai_privacy_filter';
SET duckdb_ai_base_url = 'http://localhost:8080';

SELECT
    ticket_id,
    ai_redact(internal_note) AS redacted_internal_note
FROM support_tickets;
```

Translate customer-facing text stored in columns:

```sql
SELECT
    ticket_id,
    ai_translate(subject || ': ' || body, 'Dutch') AS dutch_update
FROM support_tickets
WHERE language = 'en';
```

Create embeddings and compare text:

```sql
SELECT
    left_ticket.ticket_id AS left_ticket_id,
    right_ticket.ticket_id AS right_ticket_id,
    ai_similarity(
        left_ticket.subject || chr(10) || left_ticket.body,
        right_ticket.subject || chr(10) || right_ticket.body,
        provider := 'openai',
        model := 'text-embedding-3-small'
    ) AS similarity
FROM support_tickets AS left_ticket
JOIN support_tickets AS right_ticket
    ON left_ticket.ticket_id < right_ticket.ticket_id
ORDER BY similarity DESC;
```

Create typed columns from one prompt:

```sql
SELECT *
FROM ai_complete_record(
    'Extract a support triage profile for the query regression ticket.',
    '{
      "type": "object",
      "properties": {
        "product_area": {"type": "string"},
        "needs_engineering": {"type": "boolean"},
        "urgency_score": {"type": "integer"},
        "recommended_owner": {"type": "string"}
      },
      "required": ["product_area", "needs_engineering", "urgency_score"]
    }',
    provider := 'openai',
    model := 'gpt-4o-mini'
);
```

Generate and run read-only SQL over local tables:

```sql
SELECT summary
FROM ai_schema_prompt(
    include_tables := ['main.support_tickets'],
    sample_rows := 3
);

SELECT ai_sql(
    'Which high-priority customers have the lowest satisfaction scores?',
    include_tables := ['main.support_tickets'],
    sample_rows := 3
);

SELECT *
FROM ai_query_data(
    'Count tickets by priority and customer tier',
    include_tables := ['main.support_tickets'],
    sample_rows := 3
);
```

`ai_sql` and `ai_query_data` only accept one parser-valid read-only DuckDB
`SELECT` statement before returning or executing generated SQL.

## Highlighted Functions

These are the main functions most users should start with:

| Function | Use it for |
| --- | --- |
| `ai_complete` / `ai_try_complete` | General prompt-to-text completions from SQL, with optional row-level error capture. |
| `ai_summarize` / `ai_summarize_agg` | Summarizing one text value or a grouped set of rows. |
| `ai_classify` / `ai_classify_labels` | Assigning one or more labels from a controlled list. |
| `ai_filter` | Filtering rows with a natural-language predicate. |
| `ai_extract` | Pulling compact structured facts from text. |
| `ai_complete_record` | Returning model output as typed DuckDB columns from a JSON Schema. |
| `ai_extract_record` | Extracting typed `STRUCT` values per input row from a JSON Schema. |
| `ai_embed` / `ai_similarity` / `ai_rerank` | Creating embeddings, comparing text semantically, and LLM-reranking short candidate sets. |
| `ai_schema_prompt` | Building deterministic local table context for SQL generation. |
| `ai_sql` / `ai_query_data` | Generating, validating, and optionally running read-only DuckDB `SELECT` statements. |
| `ai_usage` | Inspecting recent model calls, latency, token estimates, and status. |

## Function Map

| Need | Functions |
| --- | --- |
| Completion text | `ai_complete`, `ai_try_complete`, `ai_completion_request_json` |
| Structured output | `ai_complete_json`, `ai_complete_record`, `ai_extract_record` |
| Text tasks | `ai_summarize`, `ai_sentiment`, `ai_fix_grammar`, `ai_redact`, `ai_translate`, `ai_classify`, `ai_classify_labels`, `ai_extract`, `ai_filter` |
| Aggregates | `ai_summarize_agg`, `ai_agg` |
| Embeddings and ranking | `ai_embed`, `ai_embedding_request_json`, `ai_similarity`, `ai_rerank` |
| SQL assistant | `ai_schema_prompt`, `ai_sql`, `ai_query_data`, `ai_explain_sql`, `ai_fix_sql` |
| SQL safety checks | `ai_is_read_only_sql`, `ai_validate_read_only_sql` |
| Provider metadata | `ai_provider_base_url`, `ai_provider_protocol`, `ai_model_prices` |
| Usage and secrets | `ai_usage`, `ai_clear_usage`, `ai_clear_cache`, `ai_secrets` |
| Local utility | `ai_count_tokens`, `ai_recommended_batch_size` |

The full SQL reference is in [`docs/functions.md`](docs/functions.md).
End-to-end setup examples for each provider are in
[`docs/provider-guides.md`](docs/provider-guides.md).

## Providers

### Local or self-hosted providers

| Provider | Use it with | Default base URL / key |
| --- | --- | --- |
| `ollama` | Local Ollama chat and embedding models | `http://localhost:11434`; optional `OLLAMA_API_KEY` |
| `openai_compatible` / `local` | vLLM, LM Studio, LiteLLM, Ollama `/v1`, or another gateway | Set `BASE_URL`; optional `OPENAI_COMPATIBLE_API_KEY` |
| `llamacpp` / `llama.cpp` | Local llama.cpp `llama-server` | `http://localhost:8080/v1`; optional `LLAMACPP_API_KEY` |
| `openai_privacy_filter` | OpenAI Privacy Filter PII redaction service | `http://localhost:8080`; optional `OPENAI_PRIVACY_FILTER_API_KEY` |

### Remote providers

| Provider | Use it with | Default base URL / key |
| --- | --- | --- |
| `azure` | Azure OpenAI deployments | Set `BASE_URL` or `AZURE_OPENAI_BASE_URL`; `AZURE_OPENAI_API_KEY` |
| `anthropic` / `claude` | Anthropic Claude models | `https://api.anthropic.com/v1`; `ANTHROPIC_API_KEY` |
| `databricks` | Databricks Model Serving and Unity AI Gateway chat endpoints | Set `BASE_URL` or `DATABRICKS_HOST`; `DATABRICKS_TOKEN` |
| `deepseek` | DeepSeek chat models | `https://api.deepseek.com`; `DEEPSEEK_API_KEY` |
| `gemini` / `gcp` / `google` | Gemini through the OpenAI-compatible endpoint | `GEMINI_API_KEY` |
| `mistral` | Mistral chat models | `https://api.mistral.ai/v1`; `MISTRAL_API_KEY` |
| `openai` | OpenAI chat and embedding models | `https://api.openai.com/v1`; `OPENAI_API_KEY` |
| `openrouter` | OpenRouter model routing | `https://openrouter.ai/api/v1`; `OPENROUTER_API_KEY` |
| `snowflake` | Snowflake Cortex REST Chat Completions API | Set `BASE_URL`, `SNOWFLAKE_ACCOUNT_URL`, or `SNOWFLAKE_ACCOUNT`; `SNOWFLAKE_PAT` |
| `zai` | Z.ai / BigModel chat models | `https://api.z.ai/api/paas/v4`; `ZAI_API_KEY` |

You can configure providers three ways:

```sql
-- Session defaults.
SET duckdb_ai_provider = 'openai';
SET duckdb_ai_model = 'gpt-4o-mini';
SET duckdb_ai_embedding_model = 'text-embedding-3-small';
SET duckdb_ai_timeout_seconds = 120;

-- Per-call overrides.
SELECT ai_complete(
    'Summarize this in one sentence.',
    provider := 'openai',
    model := 'gpt-4o-mini',
    temperature := 0.2,
    max_tokens := 128
);

-- DuckDB secrets.
CREATE OR REPLACE SECRET local_llm (
    TYPE duckdb_ai,
    AI_PROVIDER 'local',
    BASE_URL 'http://localhost:11434/v1',
    MODEL 'gemma4:e4b'
);

SELECT ai_complete('hello', secret := 'local_llm');
```

Provider-specific environment variables such as `OPENAI_API_KEY`,
`ANTHROPIC_API_KEY`, `GEMINI_API_KEY`, `DATABRICKS_TOKEN`, `SNOWFLAKE_PAT`, and
`OPENAI_PRIVACY_FILTER_API_KEY` are also supported. Generic overrides include
`DUCKDB_AI_PROVIDER`, `DUCKDB_AI_MODEL`, `DUCKDB_AI_BASE_URL`, and
`DUCKDB_AI_API_KEY`.

Use family-specific model settings when different function groups should default
to different models. These override `duckdb_ai_model`, and per-call `model := ...`
still overrides both:

```sql
SET duckdb_ai_completion_model = 'gpt-4o-mini';
SET duckdb_ai_task_model = 'gpt-4o-mini';
SET duckdb_ai_aggregate_model = 'gpt-4o-mini';
SET duckdb_ai_sql_assistant_model = 'gpt-4o';
SET duckdb_ai_embedding_model = 'text-embedding-3-small';
```

## Structured Output

Use `ai_complete_json` when you need valid JSON:

```sql
SELECT ai_complete_json(
    'Return {"name": "...", "summary": "..."} for DuckDB.',
    provider := 'openai',
    response_schema := '{
      "type": "object",
      "properties": {
        "name": {"type": "string"},
        "summary": {"type": "string"}
      },
      "required": ["name", "summary"]
    }'
);
```

Use `ai_complete_record` when you want DuckDB columns. Scalar schema properties
become `VARCHAR`, `BOOLEAN`, `BIGINT`, or `DOUBLE`; supported nested objects and
arrays become `STRUCT` and `LIST` values.

## Safety And Privacy

- Request-preview functions such as `ai_completion_request_json` and
  `ai_embedding_request_json` build provider request bodies without making a
  network call.
- API keys are resolved from environment variables or DuckDB secrets, not direct
  SQL arguments.
- Provider error messages redact the active API key before surfacing errors.
- Usage logs do not include prompt, input, or response text unless
  `duckdb_ai_log_include_text` or `DUCKDB_AI_LOG_INCLUDE_TEXT=1` is enabled.
- `duckdb_ai_allowed_hosts` and `allowed_hosts := ...` can restrict provider
  egress to a comma-separated host allowlist.
- Generated SQL is guarded by `ai_validate_read_only_sql`, which only accepts one
  read-only DuckDB `SELECT`.

By default, provider errors fail the SQL query. For exploratory workflows, use
`on_error := 'null'` to return `NULL` instead:

```sql
SELECT ai_complete(
    'Try this with a best-effort provider call.',
    provider := 'openai',
    on_error := 'null'
);
```

`fail_on_error := false` is still accepted as a compatibility alias for
`on_error := 'null'`. Use `ai_try_complete` when you need row-level error text
instead of a bare `NULL`.

## Usage And Cost Visibility

Completions, embeddings, and local `ai_schema_prompt` calls — including failed
provider calls — are kept in a local in-process ring buffer with function name,
query id, provider, latency, token, cache, retry, status, error, and cost
metadata:

```sql
SELECT * FROM ai_usage();
SELECT * FROM ai_clear_usage();
SELECT * FROM ai_clear_cache();
```

Cost estimates are opt-in. Either pass prices per call or enable the built-in
model price catalog:

```sql
SET duckdb_ai_use_builtin_model_prices = true;
SELECT * FROM ai_model_prices();
```

Provider pricing changes often, so treat the built-in catalog as a convenience
for common models rather than a billing authority.

To send usage events to your own collector:

```sql
SET duckdb_ai_log_endpoint = 'https://collector.example/ai-usage';
SET duckdb_ai_log_format = 'generic_json';
SET duckdb_ai_log_tags = 'warehouse=local,app=duckdb';
```

Use `otlp_json` when sending logs to an OpenTelemetry collector endpoint.

## Reliability Controls

Retries are disabled by default. Enable them explicitly for transient provider
errors:

```sql
SELECT ai_complete(
    'Summarize this.',
    connect_timeout_seconds := 5,
    retry_count := 2,
    retry_backoff_ms := 1000
);
```

For batch workloads, cap provider concurrency and rate:

```sql
SET duckdb_ai_max_concurrent_requests = 4;
SET duckdb_ai_min_request_interval_ms = 100;
SET duckdb_ai_token_limit_per_minute = 200000;
```

These controls apply per DuckDB database instance to completion and embedding
calls. The token limit uses the local `ai_count_tokens` estimate plus
`max_tokens` when present, or a conservative default output estimate otherwise.
Set `max_tokens` for large jobs so the runtime can pace requests against your
provider's current tokens-per-minute limit.

Response caching is opt-in and in-memory. Identical in-flight requests are
coalesced into one provider call, and cached entries can expire with a TTL:

```sql
SET duckdb_ai_cache = true;
SET duckdb_ai_cache_ttl_seconds = 3600;
SET duckdb_ai_cache_max_entries = 1024;
SELECT ai_complete('Summarize this repeated prompt.');
SELECT * FROM ai_clear_cache();
```

Provider-side prompt caching is separate and reduces cost on repeated static
prefixes (system prompts, schemas). Enable it with `prompt_cache := true` or
`SET duckdb_ai_prompt_cache = true`; the extension sends the matching cache
hints for OpenAI and Anthropic and reports cached token counts in `ai_usage()`.

Use `ai_recommended_batch_size` with a small provider-limit table to pick a
starting batch size before running a large enrichment:

```sql
CREATE OR REPLACE TABLE ai_provider_limits AS
SELECT
    'openai' AS provider,
    'gpt-4o-mini' AS model,
    200000::BIGINT AS token_limit_per_minute,
    500::BIGINT AS request_limit_per_minute;

WITH prompt_stats AS (
    SELECT avg(ai_count_tokens(subject || ': ' || body)) AS input_tokens_per_row
    FROM support_tickets
)
SELECT ai_recommended_batch_size(
    input_tokens_per_row,
    200, -- planned max_tokens per row
    token_limit_per_minute,
    request_limit_per_minute
) AS recommended_rows_per_minute
FROM prompt_stats, ai_provider_limits
WHERE provider = 'openai' AND model = 'gpt-4o-mini';
```

For production row enrichment, use `ai_try_complete` when one bad row should not
fail the full query. It returns a `STRUCT(response VARCHAR, error VARCHAR)`, so
successful rows and rejected rows can be written separately:

```sql
CREATE TEMP TABLE ticket_ai_attempts AS
SELECT
    ticket_id,
    ai_try_complete(
        subject || ': ' || body,
        provider := 'openai',
        model := 'gpt-4o-mini',
        max_tokens := 200,
        token_limit_per_minute := 200000
    ) AS result
FROM support_tickets;

CREATE OR REPLACE TABLE ticket_summaries AS
SELECT ticket_id, result.response AS summary
FROM ticket_ai_attempts
WHERE result.error IS NULL;

CREATE OR REPLACE TABLE ticket_ai_failed_rows AS
SELECT ticket_id, result.error AS error_reason, current_timestamp AS failed_at
FROM ticket_ai_attempts
WHERE result.error IS NOT NULL;
```

Use the same rejected-row `SELECT` inside `COPY (...) TO 'failed_rows.parquet'`
or an `s3://...` target when failures should live outside the DuckDB database.

## Learn More

- [`docs/functions.md`](docs/functions.md): complete SQL function reference.
- [`docs/provider-guides.md`](docs/provider-guides.md): end-to-end examples for
  every supported provider.
- [`docs/best-practices.md`](docs/best-practices.md): provider selection,
  secrets, defaults, privacy, logging, throughput, and cost guidance.
- [`docs/runtime-behavior.md`](docs/runtime-behavior.md): function stability,
  runtime state, caching, concurrency, retries, and egress allowlisting.
- [`docs/security-data-flow.md`](docs/security-data-flow.md): egress controls,
  per-function data flow, logging defaults, and proxy/TLS notes.
- [`SECURITY.md`](SECURITY.md): vulnerability reporting policy.
- [`CHANGELOG.md`](CHANGELOG.md): release notes and compatibility policy.
- [`CONTRIBUTING.md`](CONTRIBUTING.md): development workflow for contributors.
