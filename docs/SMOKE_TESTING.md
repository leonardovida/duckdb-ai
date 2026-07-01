# Smoke Testing

This extension has deterministic SQLLogic tests plus a mock HTTP provider smoke
test. Live provider checks are optional and require local services or real API
credentials.

## Build and deterministic tests

```sh
GEN=ninja make release
GEN=ninja make test
python3 test/smoke/mock_provider_smoke.py
```

The mock smoke covers OpenAI-compatible chat and embeddings, Ollama chat,
Claude messages, usage logging, retries, and provider-error redaction without
external network calls.

## Local Ollama

Start Ollama and pull a chat model:

```sh
ollama serve
ollama pull llama3.2
```

In another shell:

```sh
./build/release/duckdb -c "
LOAD duckdb_ai;
SELECT ai_complete(
    'Write one sentence about DuckDB extensions.',
    provider := 'ollama',
    model := 'llama3.2',
    timeout_seconds := 120
);
SELECT ai_complete_json(
    'Return {\"name\":\"DuckDB\",\"kind\":\"database\"} as JSON.',
    provider := 'ollama',
    model := 'llama3.2',
    response_format := 'json_object'
);
"
```

If Ollama is on a non-default host, set `OLLAMA_HOST` or pass `base_url`.

## Remote OpenAI-Compatible Provider

Use DuckDB Secrets so keys do not appear in function arguments:

```sh
./build/release/duckdb -c "
LOAD duckdb_ai;
CREATE OR REPLACE SECRET openai_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'openai',
    API_KEY '${OPENAI_API_KEY}',
    MODEL 'gpt-4o-mini'
);
SELECT ai_complete('Say hello from DuckDB SQL.', secret := 'openai_ai');
SELECT ai_embed('DuckDB vector smoke', secret := 'openai_ai')[1];
"
```

For OpenRouter, Gemini, Mistral, Zai, or DeepSeek, change `PROVIDER`,
`MODEL`, and the corresponding environment variable.

## Claude

```sh
./build/release/duckdb -c "
LOAD duckdb_ai;
CREATE OR REPLACE SECRET claude_ai (
    TYPE duckdb_ai,
    AI_PROVIDER 'anthropic',
    API_KEY '${ANTHROPIC_API_KEY}',
    MODEL 'claude-3-5-haiku-latest'
);
SELECT ai_complete(
    'Explain DuckDB in one short paragraph.',
    secret := 'claude_ai',
    max_tokens := 128
);
"
```

## Usage Log Endpoint

Point `DUCKDB_AI_LOG_ENDPOINT` or `duckdb_ai_log_endpoint` at a local
collector to inspect privacy-minimized log payloads. By default, prompt and
response text are omitted.

```sh
DUCKDB_AI_LOG_ENDPOINT='http://127.0.0.1:8080/log' \
./build/release/duckdb -c "
LOAD duckdb_ai;
SELECT ai_complete(
    'hello',
    provider := 'ollama',
    model := 'llama3.2',
    log_tags := 'smoke=local'
);
"
```

For an OpenTelemetry Collector, use the collector's OTLP HTTP logs endpoint and
set `log_format := 'otlp_json'`:

```sh
DUCKDB_AI_LOG_ENDPOINT='http://127.0.0.1:4318/v1/logs' \
./build/release/duckdb -c "
LOAD duckdb_ai;
SELECT ai_complete(
    'hello',
    provider := 'ollama',
    model := 'llama3.2',
    log_format := 'otlp_json',
    log_tags := 'smoke=otel'
);
"
```
