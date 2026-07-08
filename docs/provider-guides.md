---
sidebar_position: 3
---

# Provider guides

This page gives one simple end-to-end example for each supported provider.
Examples assume the extension is installed and loaded:

```sql
INSTALL ai FROM community;
LOAD ai;
```

(Or build from source with `GEN=ninja make release` and run
`./build/release/duckdb`.)

Each guide uses a DuckDB secret for provider, model, and base URL settings. The
API key is read from the matching environment variable when the provider call
runs. If you prefer a fully DuckDB-managed secret, add `API_KEY '...'` to the
`CREATE SECRET` statement.

After any provider call, inspect the local usage buffer:

```sql
SELECT function_name, provider, protocol, model, http_status, elapsed_ms
FROM ai_usage()
ORDER BY event_id DESC
LIMIT 1;
```

## Provider matrix

| Provider | Protocol | Default model | Credentials | Base URL behavior |
| --- | --- | --- | --- | --- |
| `ollama` | Ollama chat and embeddings | `llama3.2`; embeddings use `nomic-embed-text` | Optional `OLLAMA_API_KEY` | Defaults to `http://localhost:11434`; `OLLAMA_HOST` overrides it. |
| `openai` | OpenAI-compatible chat and embeddings | `gpt-4o-mini`; embeddings use `text-embedding-3-small` | `OPENAI_API_KEY` | Defaults to `https://api.openai.com/v1`. |
| `azure` | OpenAI-compatible chat and embeddings | `gpt-4o`; embeddings use `text-embedding-3-small` | `AZURE_OPENAI_API_KEY` | Appends `/openai/v1` to `AZURE_OPENAI_BASE_URL`, `AZURE_OPENAI_ENDPOINT`, or secret `BASE_URL` when needed. |
| `anthropic` / `claude` | Anthropic Messages | `claude-haiku-4-5` | `ANTHROPIC_API_KEY` or `CLAUDE_API_KEY` | Defaults to `https://api.anthropic.com/v1`. |
| `gemini` / `gcp` / `google` | OpenAI-compatible chat and embeddings | `gemini-3.5-flash`; embeddings use `gemini-embedding-001` | `GEMINI_API_KEY` | Defaults to Google's OpenAI-compatible endpoint. |
| `mistral` | OpenAI-compatible chat and embeddings | `mistral-small-latest`; embeddings use `mistral-embed` | `MISTRAL_API_KEY` | Defaults to `https://api.mistral.ai/v1`. |
| `zai` / `zhipu` | OpenAI-compatible chat and embeddings | `glm-4.7-flash`; embeddings use `embedding-3` | `ZAI_API_KEY` | Defaults to `https://api.z.ai/api/paas/v4`. |
| `deepseek` | OpenAI-compatible chat | `deepseek-v4-flash` | `DEEPSEEK_API_KEY` | Defaults to `https://api.deepseek.com`. |
| `openrouter` | OpenAI-compatible chat and embeddings | `openai/gpt-4o-mini`; embeddings use `openai/text-embedding-3-small` | `OPENROUTER_API_KEY` | Defaults to `https://openrouter.ai/api/v1`. |
| `databricks` | OpenAI-compatible chat | `databricks-llama-4-maverick` | `DATABRICKS_TOKEN` | Derives `/serving-endpoints` from `DATABRICKS_HOST`, or accepts full Model Serving, AI Gateway, or chat-completions URLs. |
| `snowflake` | OpenAI-compatible chat | `snowflake-llama-3.3-70b` | `SNOWFLAKE_PAT` or `SNOWFLAKE_TOKEN` | Derives `/api/v2/cortex/v1` from Snowflake account URL, host, or account id. |
| `openai_privacy_filter` | Dedicated redaction endpoint | `openai/privacy-filter` | Optional `OPENAI_PRIVACY_FILTER_API_KEY` | Defaults to `http://localhost:8080` and calls `POST /redact`. |
| `openai_compatible` / `local` | OpenAI-compatible chat and embeddings | `gpt-4o-mini`; embeddings use `text-embedding-3-small` | Optional `OPENAI_COMPATIBLE_API_KEY` | Requires `BASE_URL` or `OPENAI_COMPATIBLE_BASE_URL`. |
| `llamacpp` / `llama.cpp` | OpenAI-compatible chat and embeddings | `default` (llama-server answers with its loaded model) | Optional `LLAMACPP_API_KEY` (`llama-server --api-key`) | Defaults to `http://localhost:8080/v1`. Embeddings need `llama-server --embeddings`. |

For guidance on choosing providers, credentials, logging, cost, throughput, and
PII workflows, see [Best practices](best-practices.md).

## Ollama

Use Ollama for local models without a hosted API key.

```sh
ollama serve
ollama pull llama3.2
ollama pull nomic-embed-text
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET ollama_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'ollama',
    MODEL 'llama3.2'
);

SELECT ai_complete(
    'Write one sentence about why DuckDB is useful for analytics.',
    secret := 'ollama_ai',
    timeout_seconds := 120
) AS answer;

SELECT ai_embed(
    'DuckDB local analytics',
    secret := 'ollama_ai',
    model := 'nomic-embed-text'
)[1] AS first_embedding_value;
```

If Ollama runs on a non-default host, set `OLLAMA_HOST` before starting DuckDB or
add `BASE_URL 'http://host:11434'` to the secret.

## OpenAI

```sh
export OPENAI_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    MODEL 'gpt-4o-mini'
);

SELECT ai_complete(
    'Summarize DuckDB in one short sentence.',
    secret := 'openai_ai'
) AS answer;

SELECT ai_complete_json(
    'Return a JSON object with keys "name" and "kind" for DuckDB.',
    secret := 'openai_ai',
    response_schema := '{
      "type": "object",
      "properties": {
        "name": {"type": "string"},
        "kind": {"type": "string"}
      },
      "required": ["name", "kind"]
    }'
) AS profile_json;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'openai_ai',
    model := 'text-embedding-3-small'
)[1] AS first_embedding_value;
```

## Azure OpenAI

Use your Azure OpenAI resource URL and deployment name. The extension appends
`/openai/v1` to the base URL when needed.

```sh
export AZURE_OPENAI_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET azure_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'azure',
    BASE_URL 'https://my-resource.openai.azure.com',
    MODEL 'gpt-4o'
);

SELECT ai_complete(
    'Explain DuckDB to a data analyst in one sentence.',
    secret := 'azure_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'azure_ai',
    model := 'text-embedding-3-small'
)[1] AS first_embedding_value;
```

If your Azure deployment names differ from the model names above, use the
deployment name in `MODEL` or `model := ...`.

## Claude / Anthropic

```sh
export ANTHROPIC_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET claude_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'anthropic',
    MODEL 'claude-haiku-4-5'
);

SELECT ai_complete(
    'Explain DuckDB in one concise paragraph.',
    secret := 'claude_ai',
    max_tokens := 160
) AS answer;

SELECT ai_summarize(
    'DuckDB is an in-process analytical database built for fast local queries.',
    secret := 'claude_ai',
    max_tokens := 80
) AS summary;
```

Claude is configured for completion calls. Embeddings are not configured for this
provider.

## Gemini

Gemini uses Google's OpenAI-compatible endpoint.

```sh
export GEMINI_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET gemini_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'gemini',
    MODEL 'gemini-3.5-flash'
);

SELECT ai_complete(
    'Write one sentence about DuckDB extensions.',
    secret := 'gemini_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'gemini_ai',
    model := 'gemini-embedding-001'
)[1] AS first_embedding_value;
```

Provider aliases `gcp`, `google`, and `google_gemini` also resolve to Gemini.

## Mistral

```sh
export MISTRAL_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET mistral_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'mistral',
    MODEL 'mistral-small-latest'
);

SELECT ai_complete(
    'Give one practical use case for DuckDB and AI functions.',
    secret := 'mistral_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'mistral_ai',
    model := 'mistral-embed'
)[1] AS first_embedding_value;
```

## Z.ai

```sh
export ZAI_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET zai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'zai',
    MODEL 'glm-4.7-flash'
);

SELECT ai_complete(
    'Summarize DuckDB in one sentence.',
    secret := 'zai_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'zai_ai',
    model := 'embedding-3'
)[1] AS first_embedding_value;
```

Provider aliases `zai` and `zhipu` resolve to the same provider.

## DeepSeek

```sh
export DEEPSEEK_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET deepseek_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'deepseek',
    MODEL 'deepseek-v4-flash'
);

SELECT ai_complete(
    'Write one sentence about using SQL with language models.',
    secret := 'deepseek_ai'
) AS answer;

SELECT ai_classify(
    'Customer says the invoice is overdue.',
    'billing, support, sales, other',
    secret := 'deepseek_ai'
) AS category;
```

DeepSeek is configured for completion calls. Embeddings are not configured for
this provider.

## OpenRouter

```sh
export OPENROUTER_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET openrouter_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openrouter',
    MODEL 'openai/gpt-4o-mini'
);

SELECT ai_complete(
    'Explain DuckDB extensions in one sentence.',
    secret := 'openrouter_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'openrouter_ai',
    model := 'openai/text-embedding-3-small'
)[1] AS first_embedding_value;
```

OpenRouter model names include the upstream provider prefix, for example
`openai/gpt-4o-mini`.

## Databricks

Databricks Model Serving exposes chat endpoints through an OpenAI-compatible
API. Use a Databricks personal access token or service-principal token, and set
the model to the serving endpoint name.

```sh
export DATABRICKS_TOKEN='...'
export DATABRICKS_HOST='https://<workspace>.cloud.databricks.com'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET databricks_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'databricks',
    MODEL 'databricks-llama-4-maverick'
);

SELECT ai_complete(
    'Explain Delta Lake in one sentence.',
    secret := 'databricks_ai'
) AS answer;
```

If `BASE_URL` is omitted, set `DATABRICKS_HOST`; the extension derives
`https://<workspace>/serving-endpoints`. You can also set a secret `BASE_URL`,
`DATABRICKS_BASE_URL`, or per-call `base_url := ...` to use a full
`/serving-endpoints`, `/ai-gateway/mlflow/v1`, or `/chat/completions` endpoint.
Aliases `mosaic`, `mosaic_ai`, and `databricks_ai` resolve to `databricks`.

## Snowflake Cortex REST

Snowflake Cortex REST exposes a Chat Completions API compatible with the OpenAI
request shape. Use a Snowflake Programmatic Access Token, OAuth token, or JWT
with a role that can call Cortex REST.

```sh
export SNOWFLAKE_PAT='...'
export SNOWFLAKE_ACCOUNT_URL='https://<account-identifier>.snowflakecomputing.com'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET snowflake_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'snowflake',
    MODEL 'snowflake-llama-3.3-70b'
);

SELECT ai_complete(
    'Summarize why governed model inference matters.',
    secret := 'snowflake_ai'
) AS answer;
```

If `BASE_URL` is omitted, set `SNOWFLAKE_ACCOUNT_URL`, `SNOWFLAKE_HOST`, or
`SNOWFLAKE_ACCOUNT`; the extension derives
`https://<account>.snowflakecomputing.com/api/v2/cortex/v1`. You can also set a
secret `BASE_URL`, `SNOWFLAKE_BASE_URL`, or per-call `base_url := ...` to use a
full `/api/v2/cortex/v1` or `/chat/completions` endpoint. Snowflake model IDs
include values such as `snowflake-llama-3.3-70b`, `llama4-maverick`, and
`openai-gpt-5.1`, depending on region and model access.

## OpenAI Privacy Filter

OpenAI Privacy Filter is an open-weight PII detection and masking model. The
extension calls it through a small HTTP service so the model can run either on
the same machine as DuckDB or in your own cloud deployment.

Use local hosting when unredacted PII should not leave the machine running
DuckDB:

```sh
# Host a wrapper around the openai/privacy-filter Python package.
# The wrapper should expose POST /redact.
export OPF_CHECKPOINT="$HOME/.opf/privacy_filter"
./serve-privacy-filter --host 127.0.0.1 --port 8080
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET privacy_filter_local (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai_privacy_filter',
    BASE_URL 'http://localhost:8080'
);

SELECT ai_redact(
    'email alice@example.com token fake-token',
    secret := 'privacy_filter_local'
) AS redacted_text;
```

Use cloud hosting when multiple DuckDB clients should share one managed Privacy
Filter deployment:

```sql
CREATE OR REPLACE SECRET privacy_filter_cloud (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai_privacy_filter',
    BASE_URL 'https://privacy-filter.example.com',
    API_KEY '...'
);

SELECT ai_redact(
    internal_note,
    secret := 'privacy_filter_cloud'
) AS redacted_note
FROM support_tickets;
```

The service contract is intentionally small:

```http
POST /redact
Content-Type: application/json
Authorization: Bearer ...    # optional

{"text":"email alice@example.com","model":"openai/privacy-filter"}
```

The response should include one of `redacted_text`, `masked_text`, `text`, or
`output`:

```json
{"redacted_text":"email [PRIVATE_EMAIL]"}
```

Aliases `privacy_filter`, `pii_filter`, and `opf` resolve to
`openai_privacy_filter`. The default base URL is `http://localhost:8080`; set
`OPENAI_PRIVACY_FILTER_BASE_URL`, `DUCKDB_AI_BASE_URL`, a secret `BASE_URL`, or a
per-call `base_url := ...` to point at a different local or cloud service. For
cloud auth, use a secret `API_KEY`, `OPENAI_PRIVACY_FILTER_API_KEY`, or
`DUCKDB_AI_API_KEY`.

## OpenAI-compatible / Local gateway

Use this path for vLLM, LM Studio, LiteLLM, Ollama's `/v1` endpoint, or any
gateway that exposes an OpenAI-compatible chat API.

For local Ollama's OpenAI-compatible endpoint:

```sh
ollama serve
ollama pull llama3.2
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET local_openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'local',
    BASE_URL 'http://localhost:11434/v1',
    MODEL 'llama3.2'
);

SELECT ai_complete(
    'Write one sentence about local AI models in DuckDB.',
    secret := 'local_openai_ai'
) AS answer;
```

For a hosted OpenAI-compatible gateway, add an API key:

```sh
export OPENAI_COMPATIBLE_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD ai;

CREATE OR REPLACE SECRET gateway_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai_compatible',
    BASE_URL 'https://gateway.example/v1',
    MODEL 'provider/model-name'
);

SELECT ai_complete(
    'Write one sentence about DuckDB.',
    secret := 'gateway_ai'
) AS answer;
```

Aliases `local`, `openai_compatible`, `openai-compatible`, `local_openai`,
`local-models`, and `local_models` all use the OpenAI-compatible protocol.

## llama.cpp

Use the `llamacpp` provider (aliases `llama.cpp`, `llama-cpp`, `llama_cpp`,
`llama-server`, `llama_server`) for a local
[llama.cpp](https://github.com/ggml-org/llama.cpp) `llama-server`. It uses the
OpenAI-compatible protocol and defaults to `http://localhost:8080/v1`, so no
base URL setup is needed for a default server:

```sh
llama-server -m model.gguf --embeddings
./build/release/duckdb
```

```sql
LOAD ai;
SET duckdb_ai_provider = 'llama.cpp';

SELECT ai_complete('Write one sentence about DuckDB.') AS answer;
SELECT ai_embed('DuckDB');
```

The request `model` defaults to `default`; `llama-server` ignores it and
answers with its loaded model, so no model configuration is needed either.
Set `LLAMACPP_BASE_URL` for a non-default host or port, and `LLAMACPP_API_KEY`
when the server runs with `--api-key`. Embedding calls require starting
`llama-server` with `--embeddings`.

For throughput, start `llama-server` with parallel slots (for example
`--parallel 4`) and match `max_concurrent_requests` so DuckDB keeps every slot
busy; llama.cpp reuses cached prompt prefixes automatically, so shared system
prompts stay cheap across rows.
