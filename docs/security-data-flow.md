---
sidebar_position: 6
---

# Security and data flow

`duckdb_ai` is a network-calling DuckDB extension. Treat every provider call as
an explicit data egress path from DuckDB to the configured provider or gateway.

## Defaults

- No telemetry is sent by default.
- Usage events stay in the current DuckDB database instance unless
  `duckdb_ai_log_endpoint` or `DUCKDB_AI_LOG_ENDPOINT` is configured.
- Usage logs omit prompt, input, and response text by default.
- API keys are read from environment variables or `TYPE duckdb_ai` secrets.
- `TYPE duckdb_ai` secrets redact `API_KEY` values when listed.
- Provider error messages redact the active API key before surfacing errors.
- Provider redirects are not followed for API calls.
- Provider response parsing uses DuckDB's vendored `yyjson` parser.

## Egress controls

Use `duckdb_ai_allowed_hosts` or per-call `allowed_hosts := ...` to restrict
provider and usage-log destinations:

```sql
SET duckdb_ai_allowed_hosts = 'ai-gateway.internal,api.openai.com';
```

Allowlist entries match URL hosts. Entries may be hostnames, `host:port`, full
URLs, `*.example.com`, or `*`.

The allowlist check runs before the HTTP request, including for usage-log
endpoints.

## Data sent by function family

| Function family | Data sent |
| --- | --- |
| `ai_complete`, `ai_try_complete`, `ai_complete_json`, task wrappers, aggregates, and SQL assistant functions | Prompt text, configured system prompt, model name, response-format hints, and provider options needed by the selected provider API. |
| `ai_complete_record` | Prompt text plus the supplied JSON Schema. |
| `ai_embed` and `ai_similarity` | Input text to embed and model name. `ai_similarity` sends both input strings as separate embedding requests. |
| `ai_redact` with `openai_privacy_filter` | Raw input text and model name to `POST /redact`. |
| `ai_request_json`, `ai_embedding_request_json`, `ai_schema_prompt`, `ai_is_read_only_sql`, `ai_validate_read_only_sql`, `ai_count_tokens`, `ai_recommended_batch_size`, metadata functions, and catalog table functions | No provider network call. |
| `ai_query_data` | The natural-language question and generated schema context are sent during bind; generated SQL is validated as one read-only DuckDB `SELECT` before execution. |

## Logging

Outbound usage logs are disabled unless a log endpoint is configured. Default
payloads contain provider, protocol, model, character counts, token counts,
elapsed time, HTTP status, optional tags, and optional estimated cost. Text
fields are included only when `duckdb_ai_log_include_text` or
`DUCKDB_AI_LOG_INCLUDE_TEXT=1` is enabled.

## Proxy and TLS

Provider HTTP calls use libcurl. Standard environment variables such as
`HTTPS_PROXY`, `HTTP_PROXY`, `NO_PROXY`, `SSL_CERT_FILE`, and `CURL_CA_BUNDLE`
can be used where supported by the libcurl build.

## Vulnerabilities

Report suspected vulnerabilities using the process in the repository
`SECURITY.md`. Do not include API keys, provider credentials, private data, or
full model prompts in public issues.
