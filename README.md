# duckdb-ai: LLMs and embeddings in DuckDB SQL

Call large language models from SQL. Summarize, classify and filter rows,
extract typed fields, embed text for semantic search, and ask questions about
your tables, using local models (Ollama, llama.cpp, any OpenAI-compatible
server) or hosted providers (OpenAI, Anthropic Claude, Google Gemini,
OpenRouter, Databricks, Snowflake Cortex and more than 30 others).

[![CI](https://github.com/leonardovida/duckdb-ai/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/leonardovida/duckdb-ai/actions/workflows/MainDistributionPipeline.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

[Documentation](https://leonardovida.github.io/duckdb-ai/docs/) ·
[SQL reference](docs/functions.md) ·
[Provider setup](docs/provider-guides.md) ·
[Cookbooks](docs/cookbooks/index.md) ·
[Agent guide](docs/agent-guide.md)

<img src="docs/assets/duckdb-ai-logo.svg" alt="duckdb-ai: AI functions for DuckDB SQL" width="280">

```sql
SELECT ticket_id,
       ai_classify(body, ['billing', 'performance', 'other']) AS category,
       ai_summarize(body) AS summary
FROM support_tickets;
```

## Contents

- [Names at a glance](#names-at-a-glance)
- [Quickstart with a local model](#quickstart-with-a-local-model)
- [Connect a hosted provider](#connect-a-hosted-provider)
- [Choose a function](#choose-a-function)
- [Examples, from simple to advanced](#examples-from-simple-to-advanced)
- [Using the extension from an agent](#using-the-extension-from-an-agent)
- [Supported providers](#supported-providers)
- [Privacy, security and limits](#privacy-security-and-limits)
- [Build from source](#build-from-source)
- [Documentation](#documentation)

## Names at a glance

| What | Name |
| --- | --- |
| Repository and project | `duckdb-ai` |
| Extension (`INSTALL` / `LOAD`) | `ai` |
| SQL functions | `ai_*` |
| Settings | `duckdb_ai_*` (for example `SET duckdb_ai_provider = 'ollama'`) |
| Secret type | `TYPE duckdb_ai` |

The extension is published in the
[DuckDB community extension catalog](https://duckdb.org/community_extensions/extensions/ai)
and works anywhere DuckDB can load community extensions: the CLI, Python, and
other clients.

> [!NOTE]
> This README describes the source on `main`. The community package can lag
> behind it. Check your installed version and function list (see
> [step 1 below](#1-install-and-check-the-installed-api)) before relying on a
> newer function, and see the [changelog](CHANGELOG.md) for what changed when.

## Quickstart with a local model

This path needs no API key and sends no data off your machine.

### 1. Install and check the installed API

```sql
INSTALL ai FROM community;
LOAD ai;

-- Installed version
SELECT extension_version
FROM duckdb_extensions()
WHERE extension_name = 'ai';

-- Available functions (makes no model calls)
SELECT function_name, function_type
FROM duckdb_functions()
WHERE starts_with(function_name, 'ai_')
ORDER BY function_name;
```

`LOAD ai;` is needed once per connection. Loading the extension never calls a
model by itself.

### 2. Start Ollama and pull a model

Install [Ollama](https://ollama.com/download), then in a terminal:

```sh
ollama serve                    # skip if Ollama is already running
ollama pull qwen3.8:27b         # chat model
ollama pull nomic-embed-text    # embedding model (optional)
```

### 3. Call it from SQL

```sql
LOAD ai;

SET duckdb_ai_provider = 'ollama';
SET duckdb_ai_model = 'qwen3.8:27b';
SET duckdb_ai_embedding_model = 'nomic-embed-text';

SELECT ai_complete('Describe DuckDB in one sentence.');
```

The extension calls Ollama at `http://localhost:11434`. Set `OLLAMA_HOST`, or
pass `base_url := ...`, if it runs elsewhere. For llama.cpp, vLLM, LM Studio or
LiteLLM, see
[local and self-hosted setup](docs/provider-guides.md#openai-compatible--local-gateway).

## Connect a hosted provider

Keep API keys out of SQL. Put the key in the DuckDB process environment (or in
a secret manager that injects it), then create a named secret that holds only
the provider and model:

```sh
export OPENAI_API_KEY='...'
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    MODEL 'gpt-5.6-luna'
);

SELECT ai_complete(
    'Describe DuckDB in one sentence.',
    secret := 'openai_ai',
    max_tokens := 64
);
```

Every provider follows the same pattern: an environment variable for the key,
a secret or session setting for provider and model. The
[provider guides](docs/provider-guides.md) list the variable name, base URL and
model IDs for each one. Hosted calls send your input to that provider and may
incur charges.

### How settings combine

You can configure the provider and model in three places. For a single call,
the most specific one wins:

1. Named options on the call: `provider := ...`, `model := ...`, `secret := ...`
2. A named secret or `CREATE EXTERNAL MODEL` profile passed with `secret :=` or `profile :=`
3. Session settings: `SET duckdb_ai_provider`, `duckdb_ai_model`, and the
   family-specific `duckdb_ai_completion_model`, `duckdb_ai_task_model`,
   `duckdb_ai_embedding_model`, `duckdb_ai_sql_assistant_model`

Completion and embedding models are configured separately, so set an embedding
model before using `ai_embed` or `ai_similarity`. The full resolution order is
in [provider settings and secrets](docs/functions.md#provider-settings-and-secrets).

## Choose a function

| Task | Functions | Returns |
| --- | --- | --- |
| Prompt a model | `ai_complete`, `ai_try_complete` | text; `STRUCT(response, error)` |
| Summarize, translate, fix grammar | `ai_summarize`, `ai_translate`, `ai_fix_grammar` | text |
| Classify | `ai_classify`, `ai_classify_labels`, `ai_sentiment` | one label; a list of labels; sentiment |
| Filter or score | `ai_filter`, `ai_score` | `BOOLEAN`; `DOUBLE` from 0 to 1 |
| Extract structured data | `ai_extract_record`, `ai_complete_json`, `ai_complete_record` | typed `STRUCT` per row; validated JSON; a typed table |
| Redact personal data | `ai_redact` | text with direct identifiers masked |
| Embed and rank | `ai_embed`, `ai_similarity`, `ai_rerank` | `DOUBLE[]`; cosine similarity; LLM relevance score |
| Summarize groups | `ai_agg`, `ai_summarize_agg` (aggregates) | text per group |
| Prepare documents for RAG | `ai_generate_chunks`, `ai_prep_search`, `ai_parse_document` | chunk tables |
| Text-to-SQL | `ai_sql`, `ai_query_data`, `ai_explain_sql`, `ai_fix_sql` | SQL text; query results |
| Typed decisions at scale | `ai_decide` ([decision models](docs/provider-guides.md#decision-models): Jev, Clef, Perplexity, Ollama) | `STRUCT` of choices, scores and probabilities |
| Raw provider APIs | `ai_provider_call` | full provider JSON (tools, reasoning, streaming events) |
| Preview without calling a model | `ai_completion_request_json`, `ai_embedding_request_json`, `ai_count_tokens` | request JSON; approximate token count |
| Usage, cost and caches | `ai_usage()`, `ai_usage_summary()`, `ai_usage_totals()`, `ai_query_cache_stats()` | tables |

The [SQL reference](docs/functions.md) documents every function, its named
options (timeouts, retries, concurrency, caching, JSON Schemas, cost tracking,
logging) and its result shape.

## Examples, from simple to advanced

The examples assume the [quickstart](#quickstart-with-a-local-model) settings and
this small table:

```sql
CREATE TABLE support_tickets AS
SELECT * FROM (VALUES
    (1, 'I was charged twice for the same invoice.'),
    (2, 'My query became slow after importing more data.'),
    (3, 'Please update the billing email on our account.')
) AS t(ticket_id, body);
```

### Enrich rows

Each function runs once per row:

```sql
SELECT ticket_id,
       ai_classify(body, ['billing', 'performance', 'other']) AS category,
       ai_filter(body, 'the customer reports a bug or outage') AS is_incident,
       ai_translate(body, 'Italian') AS body_it
FROM support_tickets;
```

### Extract typed fields

`ai_extract_record` returns a `STRUCT` whose fields come from a JSON Schema, so
results can be filtered and joined like any other column:

```sql
SELECT ticket_id, r.product_area, r.urgency
FROM (
    SELECT ticket_id,
           ai_extract_record(body, '{
             "type": "object",
             "properties": {
               "product_area": {"type": "string"},
               "urgency": {"type": "integer"}
             },
             "required": ["product_area", "urgency"]
           }') AS r
    FROM support_tickets
);
```

### Semantic search with embeddings

Embed once, store the vectors, and rank with DuckDB's own list functions:

```sql
CREATE TABLE ticket_vectors AS
SELECT ticket_id, body, ai_embed(body) AS embedding
FROM support_tickets;

-- Embed the search text once, not once per row
SET VARIABLE query_embedding = ai_embed('wrong charge on my bill');

SELECT ticket_id, body,
       list_cosine_similarity(embedding, getvariable('query_embedding')) AS score
FROM ticket_vectors
ORDER BY score DESC
LIMIT 5;
```

For larger corpora, see [Lance-backed semantic search](docs/cookbooks/lance-semantic-search.md)
and the chunking helpers `ai_generate_chunks` and `ai_prep_search`.

### Summarize groups

```sql
SELECT ai_classify(body, ['billing', 'performance', 'other']) AS category,
       ai_summarize_agg(body) AS themes
FROM support_tickets
GROUP BY ALL;
```

### Ask questions about your tables

`ai_sql` returns the generated SQL for review. `ai_query_data` generates it and
runs it. Both reject anything other than a single read-only `SELECT`:

```sql
SELECT ai_sql('how many tickets mention billing?', include_tables := ['support_tickets']);

SELECT *
FROM ai_query_data(
    'count tickets by first word of the body',
    include_tables := ['support_tickets'],
    fix_attempts := 2   -- feed bind errors back to the model up to twice
);
```

`include_tables` limits which schemas are described to the model. Add
`sample_rows := N` only when it is acceptable to send sample data to the
provider.

### Run large batch jobs

For production runs, keep failures per row instead of failing the query, bound
the request rate, and save results before you read them:

```sql
CREATE TABLE ticket_summaries AS
SELECT ticket_id,
       ai_try_complete(
           'Summarize this support ticket in one sentence: ' || body,
           max_tokens := 128,
           retry_count := 3,
           max_concurrent_requests := 8
       ) AS result
FROM (SELECT * FROM support_tickets ORDER BY ticket_id LIMIT 1000);

-- Successes and failures, read from the saved table without new model calls
SELECT ticket_id, result.response FROM ticket_summaries WHERE result.error IS NULL;
SELECT ticket_id, result.error    FROM ticket_summaries WHERE result.error IS NOT NULL;

-- Calls, tokens, retries, cache hits and estimated cost
SELECT * FROM ai_usage_summary();
```

Related cookbooks:
[chaining several AI steps](docs/cookbooks/chain-ai-steps.md),
[production batch enrichment](docs/cookbooks/production-batch-enrichment.md),
[resumable enrichment](docs/cookbooks/resumable-enrichment.md),
[enriching Postgres or MySQL rows](docs/cookbooks/source-database-enrichment.md),
[audited lakehouse output](docs/cookbooks/audited-lakehouse-output.md), and
[usage and cost monitoring](docs/cookbooks/usage-cost-observability.md).

### Use provider-native features

`request_options` passes provider-specific fields (reasoning effort, thinking,
`top_p`) to the plain completion functions. `ai_provider_call` sends a complete
native request body and returns the raw response, including tool calls and
reasoning output. The extension never executes tools; your code runs them and
sends the results back.

```sql
SELECT ai_complete('Explain the proof.', provider := 'deepseek',
                   request_options := '{"reasoning_effort":"high"}');
```

See [native provider JSON](docs/functions.md#native-provider-json) and
[text API coverage](docs/provider-guides.md#text-api-coverage) for what each
provider supports.

## Using the extension from an agent

Agents write most of the SQL that uses this extension. These rules avoid the
common mistakes. The [agent guide](docs/agent-guide.md) covers each one in
more detail.

**Before making a model call**

- Check what is installed (`duckdb_extensions()`, `duckdb_functions()`). Do not
  assume a function from this README exists in the user's version.
- Preview the request offline. This needs no key and makes no network call:

  ```sql
  SELECT ai_completion_request_json('Reply OK', provider := 'openai',
                                    model := 'gpt-5.6-luna', max_tokens := 64);
  ```

  A correct preview proves the request shape, not authentication or model
  availability.
- Never put API keys in SQL, prompts, `request_options` or files. Use
  environment variables or `TYPE duckdb_ai` secrets, and pass
  `secret := 'name'` explicitly so the configuration in use is clear.

**When writing queries**

- Every model function call is one or more model requests per row. Limit the
  input in a subquery (`FROM (SELECT ... LIMIT 100)`) before scaling up.
  An outer `OFFSET` can still evaluate model calls for the skipped rows.
- Model functions are `VOLATILE`. Running the same query twice calls the model
  twice. Save results with `CREATE TABLE ... AS` before reading several fields
  or splitting successes from failures. `cache := true` enables an opt-in
  in-memory response cache.
- Table functions (`ai_complete_record`, `ai_query_data`, `ai_usage`,
  `ai_schema_prompt`) go in `FROM`. `ai_extract_record` and `ai_decide` are
  scalars that return a `STRUCT`, and their schema arguments must be constants.
- Use `ai_try_complete` or `on_error := 'null'` so that one bad row does not
  abort a batch.
- Set a bounded `max_tokens` on live checks. Reasoning models can spend the
  whole budget before writing visible text; use their reasoning controls instead
  of raising the cap repeatedly.

**After the call**

- Inspect `ai_usage()` for status, latency, token counts and errors. A
  missing-key or out-of-credit error is not a successful verification.
- Treat generated SQL as untrusted. Read-only validation is not a sandbox; rely
  on DuckDB permissions for real access control.

## Supported providers

Model IDs are passed through to the provider unchanged. The
[provider matrix](docs/provider-guides.md#provider-matrix) lists exact provider
names, aliases, credentials, endpoints, default models and embedding support.

| Family | Providers |
| --- | --- |
| Local and self-hosted | Ollama, llama.cpp, any OpenAI-compatible server (vLLM, LM Studio, LiteLLM), OpenAI Privacy Filter |
| Hosted models | OpenAI, Anthropic Claude, Google Gemini, Mistral, DeepSeek, xAI, Cohere, Perplexity, Groq, Cerebras, Fireworks AI, Together AI, DeepInfra, Hugging Face, NVIDIA NIM, Nebius, SambaNova, SiliconFlow |
| Cloud platforms and gateways | Azure OpenAI, Amazon Bedrock, Google Vertex AI, Cloudflare Workers AI, Databricks, Snowflake Cortex, OpenRouter, Vercel AI Gateway, Poe |
| Additional model platforms | Alibaba DashScope (Qwen), Moonshot (Kimi), MiniMax, Z.ai (GLM), Tencent Hunyuan, Baidu Qianfan (ERNIE), StepFun, Volcengine (Doubao) |
| Decision models, through `ai_decide` | TypeSafe Jev, Cloudflare Clef, Perplexity Decider, Ollama (Nimble, Tev1), any `/v1/systemone` endpoint |

Provider capabilities differ: not every provider offers embeddings, JSON
Schema enforcement, or every native API. Tests use local mocks of each
provider's wire format; they do not prove that every provider and model
combination supports every feature.

## Privacy, security and limits

- **Where data goes.** Model functions send their inputs to the configured
  endpoint. With a local model server and outbound logging off, inference stays
  on your machine. `ai_redact` also sends the original text to its provider, so
  use a local endpoint when the raw text must not leave the machine.
- **Credentials** come only from environment variables or DuckDB secrets.
  `ai_secrets()` lists configured secrets with credentials redacted.
- **Egress control.** `allowed_hosts` (or `SET duckdb_ai_allowed_hosts`)
  restricts which hosts the extension may contact.
- **Usage logs** stay in memory unless you configure an external collector, and
  they omit prompt and response text by default. Cost figures are estimates,
  not provider billing.
- **Retries and caching** are off unless you turn them on.
- **Generated SQL** is checked to be one read-only `SELECT`. Review it and use
  DuckDB access controls before running it.
- **Out of scope.** The extension does not generate images, audio or video, and
  it does not run a tool-calling agent loop.

Read [security and data flow](docs/security-data-flow.md),
[runtime behavior](docs/runtime-behavior.md) and
[best practices](docs/best-practices.md) for timeouts, rate limits, concurrency,
caching, logging and egress allowlists. Report vulnerabilities through the
[security policy](SECURITY.md).

## Build from source

You need a C++ toolchain, CMake, Ninja and libcurl development headers.

```sh
git clone --recurse-submodules https://github.com/leonardovida/duckdb-ai.git
cd duckdb-ai
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
./build/release/duckdb
```

The tests use deterministic fixtures and local HTTP mocks, so they need no API
keys. Contributors and coding agents working on the source should read
[AGENTS.md](AGENTS.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

- [Agent guide](docs/agent-guide.md): discover the installed API, configure
  providers, preview requests and verify calls.
- [SQL reference](docs/functions.md): signatures, named options, examples and
  result shapes.
- [Provider guides](docs/provider-guides.md): setup for every local, hosted and
  gateway provider.
- [Cookbooks](docs/cookbooks/index.md): batch enrichment from Parquet or S3,
  Postgres and MySQL inputs, typed records, document intake, embeddings,
  semantic search with Lance, Jev decisions and text-to-SQL.
- [Runtime behavior](docs/runtime-behavior.md): caching, concurrency, retries,
  cancellation, token limits and context sizes.
- [Changelog](CHANGELOG.md) and [releases](https://github.com/leonardovida/duckdb-ai/releases).

Licensed under the [MIT license](LICENSE).
