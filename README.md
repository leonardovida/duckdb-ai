# duckdb-ai: DuckDB AI extension for LLMs in SQL

Run large language models (LLMs) directly from DuckDB SQL. Summarize and classify
text, extract structured JSON, generate embeddings for semantic search and RAG,
and ask questions about tables with text-to-SQL.

Use local models through Ollama, llama.cpp, or an OpenAI-compatible server, or
connect to OpenAI, Anthropic Claude, Google Gemini, OpenRouter, Databricks,
Snowflake Cortex, and other hosted providers.

Provider development focuses on DeepSeek, Qwen, GLM/Z.ai, Kimi, MiniMax,
Tencent HY3/HY4, and Xiaomi MiMo. The [text API coverage guide](docs/provider-guides.md#text-api-coverage)
documents native JSON requests, reasoning/tool-call exchange, embeddings and
reranking, including tested scope and provider-specific limitations.

[Documentation](https://leonardovida.github.io/duckdb-ai/docs/) ·
[Agent guide](docs/agent-guide.md) ·
[SQL reference](docs/functions.md) ·
[Provider setup](docs/provider-guides.md) ·
[Cookbooks](docs/cookbooks/index.md)

[![CI](https://github.com/leonardovida/duckdb-ai/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/leonardovida/duckdb-ai/actions/workflows/MainDistributionPipeline.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

<img src="docs/assets/duckdb-ai-logo.svg" alt="duckdb-ai: AI functions for DuckDB SQL" width="280">

## Start here: agents and integrations

- **Package name:** `duckdb-ai`. **DuckDB extension name:** `ai`.
  **SQL functions:** `ai_*`. **Settings and secret type:** `duckdb_ai`.
- Install with `INSTALL ai FROM community;`, then `LOAD ai;` in each connection.
  The same SQL works in the DuckDB CLI and clients that support loading this extension,
  including Python.
- Read the [agent guide](docs/agent-guide.md) for version checks, API discovery,
  credential handling, and verification without model calls.
- Use the [SQL reference](docs/functions.md) for signatures and result types.
  Use the [provider guide](docs/provider-guides.md) for endpoints, model IDs, and
  environment variables. Provider capabilities differ.
- This README describes the source checkout. Community packages can lag source
  changes; inspect your installed version before using a newer function.
  Repository contributors should also read [AGENTS.md](AGENTS.md) and
  [CONTRIBUTING.md](CONTRIBUTING.md).

## Install the DuckDB AI extension

Install a compatible build from the
[DuckDB community extension catalog](https://duckdb.org/community_extensions/extensions/ai):

```sql
INSTALL ai FROM community;
LOAD ai;

SELECT extension_name, extension_version, loaded
FROM duckdb_extensions()
WHERE extension_name = 'ai';
```

Check available functions without sending data to a model:

```sql
SELECT function_name, function_type
FROM duckdb_functions()
WHERE starts_with(function_name, 'ai_')
ORDER BY function_name;
```

## Run a local LLM with Ollama

Install [Ollama](https://ollama.com/download). Start its server if it is not already
running; leave it running while you use DuckDB:

```sh
ollama serve
```

In another terminal, download the example model:

```sh
ollama pull qwen3.8:27b
```

In DuckDB:

```sql
LOAD ai;

SET duckdb_ai_provider = 'ollama';
SET duckdb_ai_model = 'qwen3.8:27b';

SELECT ai_complete('Describe DuckDB in one sentence.');
```

The generated answer varies. This example calls the local server at
`http://localhost:11434`; no hosted-provider API key is needed.
For llama.cpp, vLLM, LM Studio, or LiteLLM, see
[local and self-hosted provider setup](docs/provider-guides.md#openai-compatible--local-gateway).

## Use OpenAI or another hosted model

Provide `OPENAI_API_KEY` to the DuckDB process through your environment or secret
manager. Choose a model available to your account, then configure a named DuckDB
secret without placing the API key in the SQL:

```sql
LOAD ai;

CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    MODEL 'gpt-4o-mini'
);

SELECT ai_complete(
    'Describe DuckDB in one sentence.',
    provider := 'openai',
    secret := 'openai_ai',
    max_tokens := 64
);
```

Hosted calls send input to the selected provider and may incur charges.
For Claude, Gemini, Databricks, Snowflake, or another endpoint, follow the
[provider-specific setup guide](docs/provider-guides.md).

## Choose a SQL function

| Task | Functions and result |
| --- | --- |
| Call an LLM | `ai_complete` → text; `ai_try_complete` → `STRUCT(response, error)` |
| Summarize or translate | `ai_summarize`, `ai_translate` → text |
| Classify text | `ai_classify` → one label; `ai_classify_labels` → label list |
| Filter with natural language | `ai_filter` → boolean |
| Extract structured data | `ai_complete_json` → validated JSON; `ai_extract_record` → a typed `STRUCT` per row |
| Return typed columns | `ai_complete_record` → table; call it from `FROM` |
| Embed and rank text | `ai_embed` → `DOUBLE[]`; `ai_similarity` → cosine similarity; `ai_rerank` → an LLM relevance score |
| Summarize groups | `ai_agg`, `ai_summarize_agg` → text per group |
| Prepare RAG documents | `ai_generate_chunks`, `ai_prep_search` → chunk tables |
| Generate SQL | `ai_sql` → SQL text; `ai_query_data` → executes a validated read-only `SELECT` |
| Inspect usage | `ai_usage()`, `ai_usage_summary()` → tables of calls, tokens, errors, and cost metadata |

See the [full function reference](docs/functions.md) for JSON Schemas, options,
redaction, SQL repair, model profiles, and experimental classification.

## Enrich table rows with AI

After the Ollama setup above, this example creates its own input data:

```sql
CREATE TEMP TABLE tickets AS
SELECT * FROM (VALUES
    (1, 'I was charged twice for the same invoice.'),
    (2, 'My query became slow after importing more data.')
) AS input(ticket_id, body);

SELECT ticket_id,
       ai_classify(body, 'billing, performance, other') AS category
FROM tickets;
```

Each result is a model-selected label. Start with a small input table before
scaling to a production dataset.

For batch jobs, `ai_try_complete` preserves row-level errors. Materialize its
result before reading the response and error fields in separate queries.
See [production batch enrichment](docs/cookbooks/production-batch-enrichment.md)
and [usage and cost monitoring](docs/cookbooks/usage-cost-observability.md).
For local jobs that need to survive process restarts, try the source-checkout
[resumable enrichment example](docs/cookbooks/resumable-enrichment.md).

## Supported providers and gateways

The extension supports these provider families; the
[provider matrix](docs/provider-guides.md#provider-matrix) lists exact identifiers,
credentials, endpoints, and embedding availability.

- **Local/self-hosted:** Ollama, llama.cpp, OpenAI-compatible gateways, and the
  repository-defined OpenAI Privacy Filter REST wrapper.
- **Hosted models:** OpenAI, Anthropic Claude, Google Gemini, Mistral, DeepSeek,
  xAI, Cohere, Groq, Cerebras, Fireworks AI, Together AI, DeepInfra, Hugging Face,
  NVIDIA NIM, Nebius Token Factory, SambaNova, and SiliconFlow.
- **Structured decisions:** TypeSafe Jev for native choices, scores, and yes/no
  probabilities. See the [Jev cookbook](docs/cookbooks/jev-decisions.md) for
  multi-question evaluation in one request.
- **Cloud and routing:** Azure OpenAI, Amazon Bedrock, Google Vertex AI,
  Cloudflare Workers AI, Databricks Model Serving / Unity AI Gateway,
  Snowflake Cortex, OpenRouter, Vercel AI Gateway, and Poe.
- **Additional model platforms:** Alibaba DashScope / Qwen, Moonshot / Kimi,
  MiniMax, Z.ai / GLM, Tencent Hunyuan, Baidu Qianfan / ERNIE, StepFun,
  and Volcengine / Doubao.

Model IDs are passed through to the provider. Chat and embedding models are
selected separately. OpenAI-compatible calls support `request_options` for
additional provider fields; see the [OpenRouter routing guide](docs/provider-guides.md#openrouter).
Provider-specific discovery helpers, where available in the installed version,
may fetch a current catalog over the network. Mock coverage checks protocol
behavior; it does not prove account access or that every provider/model
combination supports every feature.

## Data privacy, reliability, and limits

Model functions send their inputs to the configured endpoint. With a loopback
model server and outbound logging disabled, inference stays local. Redaction
also sends the original input to its configured provider; choose a local
endpoint when the original text must remain on your machine.

Credentials come from environment variables or DuckDB secrets. Usage stays
in memory unless you configure an external log collector; logs omit text by
default. Cost estimates are optional and are not a replacement for provider
billing. Retries and response caching are opt-in.

Generated SQL is checked as a single read-only `SELECT`. Review it and apply
DuckDB access controls before executing it: read-only SQL validation is not
a sandbox. The extension does not provide image, video, or audio generation
APIs or execute a model tool-call loop.

Read [security and data flow](docs/security-data-flow.md),
[runtime controls](docs/runtime-behavior.md), and
[production best practices](docs/best-practices.md) for egress allowlists,
timeouts, concurrency, rate limits, caching, and logging.

## Build and test from source

Clone this repository with submodules, then build with a C++ toolchain, CMake,
Ninja, and libcurl development dependencies:

```sh
git submodule update --init --recursive
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```

Run the built shell with `./build/release/duckdb`.
The default tests use deterministic fixtures and local HTTP mocks; they do not
need paid API keys. See [contributor instructions](CONTRIBUTING.md) and the
provider smoke scripts in `test/smoke/` for deterministic contract checks.

## Documentation and examples

- [Agent integration guide](docs/agent-guide.md): discover the installed API and verify calls.
- [SQL function reference](docs/functions.md): signatures, parameters, examples, and result types.
- [Provider setup](docs/provider-guides.md): configure local models, hosted APIs, and gateways.
- [Cookbooks](docs/cookbooks/index.md): Parquet/S3 enrichment, Postgres/MySQL inputs,
  structured records, document intake, embeddings, semantic search with Lance, and text-to-SQL.
- [Release notes](CHANGELOG.md) and [published releases](https://github.com/leonardovida/duckdb-ai/releases): check version-specific changes.
- [Security policy](SECURITY.md): report vulnerabilities.
- [MIT license](LICENSE).
