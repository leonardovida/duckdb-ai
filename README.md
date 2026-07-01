# duckdb_ai

Run AI workflows directly inside DuckDB SQL.

`duckdb_ai` adds SQL functions for completions, structured JSON extraction,
embeddings, natural-language filters, SQL generation, and usage tracking. Use it
when you want to enrich local DuckDB analytics with models while keeping the
workflow in SQL.

```sql
LOAD duckdb_ai;

SELECT ai_summarize(
    'DuckDB is an analytical database built for fast local queries.',
    provider := 'ollama',
    model := 'llama3.2'
);
```

## What You Can Do

- Summarize, translate, redact, classify, and extract text from SQL queries.
- Generate valid JSON or typed DuckDB columns from model output.
- Create embeddings and compare text with cosine similarity.
- Ask questions about local tables and generate read-only DuckDB `SELECT`
  statements.
- Route model calls across local Ollama, hosted providers, and
  OpenAI-compatible gateways.
- Store credentials in DuckDB secrets instead of passing API keys through SQL
  function arguments.
- Inspect local usage events and optionally send privacy-minimized usage logs to
  your own collector.

## Status

This is a preview extension. The current repository is source-first: public
binary installation is not configured yet. See
[`docs/DISTRIBUTION.md`](docs/DISTRIBUTION.md) for the current distribution
stance.

## Quick Start

Build the extension once, then run the bundled DuckDB shell:

```sh
git submodule update --init --recursive
GEN=ninja make release
./build/release/duckdb
```

Inside DuckDB:

```sql
LOAD duckdb_ai;
SELECT ai_provider_protocol('openai');
```

For a local model with Ollama:

```sh
ollama serve
ollama pull llama3.2
```

```sql
LOAD duckdb_ai;

SET duckdb_ai_provider = 'ollama';
SET duckdb_ai_model = 'llama3.2';

SELECT ai_complete('Write one sentence about DuckDB.');
```

For OpenAI, store the API key in a DuckDB secret:

```sql
LOAD duckdb_ai;

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

Summarize support notes by customer:

```sql
SELECT
    customer_id,
    ai_summarize_agg(note ORDER BY created_at) AS summary
FROM support_notes
GROUP BY customer_id;
```

Classify messages:

```sql
SELECT
    message_id,
    ai_classify(body, 'billing, support, sales, other') AS category
FROM inbox;
```

Filter rows with a natural-language predicate:

```sql
SELECT *
FROM tickets
WHERE ai_filter(description, 'mentions an urgent billing problem');
```

Extract structured data as typed columns:

```sql
SELECT *
FROM ai_complete_record(
    'Extract a company profile for DuckDB.',
    '{
      "type": "object",
      "properties": {
        "name": {"type": "string"},
        "category": {"type": "string"},
        "confidence": {"type": "number"}
      },
      "required": ["name", "category"]
    }',
    provider := 'openai',
    model := 'gpt-4o-mini'
);
```

Create embeddings and compare text:

```sql
SELECT ai_similarity(
    'DuckDB analytics',
    'analytical SQL database',
    provider := 'openai',
    model := 'text-embedding-3-small'
);
```

Generate and run read-only SQL over local tables:

```sql
SELECT summary
FROM ai_schema_prompt(include_tables := ['main.orders']);

SELECT ai_sql(
    'count orders by status',
    schema_context := (
        SELECT summary
        FROM ai_schema_prompt(include_tables := ['main.orders'])
    )
);

SELECT *
FROM ai_query_data(
    'count orders by status',
    schema_context := (
        SELECT summary
        FROM ai_schema_prompt(include_tables := ['main.orders'])
    )
);
```

`ai_sql` and `ai_query_data` only accept one parser-valid read-only DuckDB
`SELECT` statement before returning or executing generated SQL.

## Function Map

| Need | Functions |
| --- | --- |
| Completion text | `ai_complete`, `ai_request_json` |
| Structured output | `ai_complete_json`, `ai_complete_record` |
| Text tasks | `ai_summarize`, `ai_sentiment`, `ai_fix_grammar`, `ai_redact`, `ai_translate`, `ai_classify`, `ai_extract`, `ai_filter` |
| Aggregates | `ai_summarize_agg`, `ai_agg` |
| Embeddings | `ai_embed`, `ai_embedding_request_json`, `ai_similarity` |
| SQL assistant | `ai_schema_prompt`, `ai_sql`, `ai_query_data`, `ai_explain_sql`, `ai_fix_sql`, `ai_fix_sql_line` |
| SQL safety checks | `ai_is_read_only_sql`, `ai_validate_read_only_sql` |
| Provider metadata | `ai_provider_base_url`, `ai_provider_protocol`, `ai_models` |
| Usage and secrets | `ai_usage`, `ai_clear_usage`, `ai_secrets` |
| Local utility | `ai_count_tokens` |

The full SQL reference is in [`docs/functions.md`](docs/functions.md).
End-to-end setup examples for each provider are in
[`docs/provider-guides.md`](docs/provider-guides.md).

## Providers

| Provider | Use it with | Default base URL / key |
| --- | --- | --- |
| `ollama` | Local Ollama chat and embedding models | `http://localhost:11434`; optional `OLLAMA_API_KEY` |
| `openai` | OpenAI chat and embedding models | `https://api.openai.com/v1`; `OPENAI_API_KEY` |
| `azure` | Azure OpenAI deployments | Set `BASE_URL` or `AZURE_OPENAI_BASE_URL`; `AZURE_OPENAI_API_KEY` |
| `claude` / `anthropic` | Anthropic Claude models | `https://api.anthropic.com/v1`; `ANTHROPIC_API_KEY` |
| `gemini` / `gcp` / `google` | Gemini through the OpenAI-compatible endpoint | `GEMINI_API_KEY` |
| `mistral` | Mistral chat models | `https://api.mistral.ai/v1`; `MISTRAL_API_KEY` |
| `zai` | Z.ai / BigModel chat models | `https://open.bigmodel.cn/api/paas/v4`; `ZAI_API_KEY` |
| `deepseek` | DeepSeek chat models | `https://api.deepseek.com`; `DEEPSEEK_API_KEY` |
| `openrouter` | OpenRouter model routing | `https://openrouter.ai/api/v1`; `OPENROUTER_API_KEY` |
| `openai_compatible` / `local` | vLLM, LM Studio, LiteLLM, Ollama `/v1`, or another gateway | Set `BASE_URL`; optional `OPENAI_COMPATIBLE_API_KEY` |

You can configure providers three ways:

```sql
-- Session defaults.
SET duckdb_ai_provider = 'openai';
SET duckdb_ai_model = 'gpt-4o-mini';
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
    MODEL 'llama3.2'
);

SELECT ai_complete('hello', secret := 'local_llm');
```

Provider-specific environment variables such as `OPENAI_API_KEY`,
`ANTHROPIC_API_KEY`, and `GEMINI_API_KEY` are also supported. Generic overrides
include `DUCKDB_AI_PROVIDER`, `DUCKDB_AI_MODEL`, `DUCKDB_AI_BASE_URL`, and
`DUCKDB_AI_API_KEY`.

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

- Request-preview functions such as `ai_request_json` and
  `ai_embedding_request_json` build provider request bodies without making a
  network call.
- API keys are resolved from environment variables or DuckDB secrets, not direct
  SQL arguments.
- Provider error messages redact the active API key before surfacing errors.
- Usage logs do not include prompt, input, or response text unless
  `duckdb_ai_log_include_text` or `DUCKDB_AI_LOG_INCLUDE_TEXT=1` is enabled.
- Generated SQL is guarded by `ai_validate_read_only_sql`, which only accepts one
  read-only DuckDB `SELECT`.

By default, provider errors fail the SQL query. For exploratory workflows, use
`fail_on_error := false` to return `NULL` instead:

```sql
SELECT ai_complete(
    'Try this with a best-effort provider call.',
    provider := 'openai',
    fail_on_error := false
);
```

## Usage And Cost Visibility

Successful completions, embeddings, and local `ai_schema_prompt` calls are kept
in a local in-process ring buffer:

```sql
SELECT * FROM ai_usage();
SELECT * FROM ai_clear_usage();
```

Cost estimates are opt-in. Either pass prices per call or enable the built-in
model price catalog:

```sql
SET duckdb_ai_use_builtin_model_prices = true;
SELECT * FROM ai_models();
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
    retry_count := 2,
    retry_backoff_ms := 1000
);
```

For batch workloads, cap provider concurrency and rate:

```sql
SET duckdb_ai_max_concurrent_requests = 4;
SET duckdb_ai_min_request_interval_ms = 100;
```

These controls apply to completion and embedding calls.

## Learn More

- [`docs/functions.md`](docs/functions.md): complete SQL function reference.
- [`docs/provider-guides.md`](docs/provider-guides.md): end-to-end examples for
  every supported provider.
- [`docs/SMOKE_TESTING.md`](docs/SMOKE_TESTING.md): local mock-provider, Ollama,
  and remote-provider smoke checks.
- [`docs/DISTRIBUTION.md`](docs/DISTRIBUTION.md): current install and release
  stance.
- [`CONTRIBUTING.md`](CONTRIBUTING.md): development workflow for contributors.
