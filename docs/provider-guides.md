---
sidebar_position: 3
---

# Provider guides

This page gives one simple end-to-end example for each supported provider.
Examples assume you have built the extension and are running the bundled DuckDB
shell:

```sh
GEN=ninja make release
./build/release/duckdb
```

Each guide uses a DuckDB secret for provider, model, and base URL settings. The
API key is read from the matching environment variable when the provider call
runs. If you prefer a fully DuckDB-managed secret, add `API_KEY '...'` to the
`CREATE SECRET` statement.

After any provider call, inspect the local usage buffer:

```sql
SELECT provider, protocol, model, http_status, elapsed_ms
FROM ai_usage()
ORDER BY event_id DESC
LIMIT 1;
```

## Ollama

Use Ollama for local models without a hosted API key.

```sh
ollama serve
ollama pull llama3.2
ollama pull nomic-embed-text
./build/release/duckdb
```

```sql
LOAD duckdb_ai;

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
LOAD duckdb_ai;

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
LOAD duckdb_ai;

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
LOAD duckdb_ai;

CREATE OR REPLACE SECRET claude_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'anthropic',
    MODEL 'claude-3-5-haiku-latest'
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
LOAD duckdb_ai;

CREATE OR REPLACE SECRET gemini_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'gemini',
    MODEL 'gemini-2.5-flash'
);

SELECT ai_complete(
    'Write one sentence about DuckDB extensions.',
    secret := 'gemini_ai'
) AS answer;

SELECT ai_embed(
    'DuckDB vector search',
    secret := 'gemini_ai',
    model := 'text-embedding-004'
)[1] AS first_embedding_value;
```

Provider aliases `gcp`, `google`, and `google_gemini` also resolve to Gemini.

## Mistral

```sh
export MISTRAL_API_KEY='...'
./build/release/duckdb
```

```sql
LOAD duckdb_ai;

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
LOAD duckdb_ai;

CREATE OR REPLACE SECRET zai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'zai',
    MODEL 'glm-4-flash'
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
LOAD duckdb_ai;

CREATE OR REPLACE SECRET deepseek_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'deepseek',
    MODEL 'deepseek-chat'
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
LOAD duckdb_ai;

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
LOAD duckdb_ai;

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
LOAD duckdb_ai;

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
