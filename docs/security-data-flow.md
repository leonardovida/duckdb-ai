---
sidebar_position: 6
---

# Security and data flow

The `ai` extension is a network-calling DuckDB extension. Treat every provider call as
an explicit data egress path from DuckDB to the configured provider or gateway.

## Defaults

- No telemetry is sent by default.
- Usage events stay in the current DuckDB database instance unless
  `duckdb_ai_log_endpoint` or `DUCKDB_AI_LOG_ENDPOINT` is configured.
- Usage logs omit prompt, input, and response text by default.
- API keys are read from environment variables or `TYPE duckdb_ai` secrets.
- External model objects contain only safe routing metadata and the name of a
  `TYPE duckdb_ai` credential secret; they never contain credential material.
- `TYPE duckdb_ai` secrets redact `API_KEY` values when listed.
- Provider error messages redact the active API key before surfacing errors.
- Provider redirects are not followed for API calls.
- Provider and log URLs are restricted to HTTP(S), and URLs with embedded
  credentials or raw control characters are rejected.
- HTTP response bodies are bounded to 64 MiB by default; use
  `DUCKDB_AI_MAX_RESPONSE_BYTES` to configure a different bound up to 1 GiB.
- Caller-controlled HTTP header values reject CR, LF, and NUL characters.
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

The URL validation still runs when no host allowlist is configured. An
allowlist controls which HTTP(S) hosts are reachable; it does not enable other
libcurl protocols.

## Data sent by function family

| Function family | Data sent |
| --- | --- |
| `ai_complete`, `ai_try_complete`, `ai_complete_json`, `ai_rerank`, task wrappers, aggregates, and SQL assistant functions | Prompt text, configured system prompt, model name, response-format hints, and provider options needed by the selected provider API. |
| `ai_complete_record` | Prompt text plus the supplied JSON Schema. |
| `ai_embed`, `ai_similarity`, and optimized classification | Input text to embed and model name. Inputs with the same options are packed by count, estimated tokens, and bytes. `ai_similarity` uses a bounded query-local cache to send each distinct value once across vector chunks. |
| `ai_redact` with `openai_privacy_filter` | Raw input text and model name to `POST /redact`. |
| `ai_generate_chunks`, `ai_prep_search(..., enrich := false)`, `ai_completion_request_json`, `ai_embedding_request_json`, `ai_schema_prompt`, `ai_is_read_only_sql`, `ai_validate_read_only_sql`, `ai_count_tokens`, `ai_recommended_batch_size`, metadata functions, and catalog table functions | No provider network call. |
| `ai_query_data` | The natural-language question and generated schema context are sent during bind; generated SQL is validated as one read-only DuckDB `SELECT` before execution. SQL assistant schema context is sent in the provider system message when the provider protocol supports it. Successful generated SQL is cached in memory for repeated binds with the same question, schema context, and output-affecting options. |
| `ai_parse_document` | Binary document content encoded as base64, MIME type, parser profile, and optional page selection are sent to `DUCKDB_AI_CONTROL_PLANE_URL`. |
| Endpoint provisioning functions | Safe external-model metadata, operation IDs, and explicit spend ceilings are sent to `DUCKDB_AI_CONTROL_PLANE_URL`. Cloud credentials are not read or sent by the extension. |

## Optional control plane

Endpoint provisioning and remote document parsing are disabled until
`DUCKDB_AI_CONTROL_PLANE_URL` is configured. The optional
`DUCKDB_AI_CONTROL_PLANE_TOKEN` is sent as a bearer token and should be scoped to
the companion service, not to a cloud provider. Applied provisioning requires
an explicit `dry_run := false` and positive hourly spend ceiling. Registration,
query binding, and first inference never provision resources.

## Logging

Outbound usage logs are disabled unless a log endpoint is configured. Default
payloads contain function name, query id, provider, protocol, model, character
counts, token counts, elapsed time, HTTP status, optional tags, and optional
estimated cost. Text fields are included only when `duckdb_ai_log_include_text` or
`DUCKDB_AI_LOG_INCLUDE_TEXT=1` is enabled.

When strict logging is disabled, outbound usage logs are queued in memory and
sent asynchronously on a best-effort basis. Enabling `duckdb_ai_log_strict` or
`DUCKDB_AI_LOG_STRICT=1` posts logs synchronously and fails the SQL query if the
collector request fails.

## Proxy and TLS

Provider HTTP calls use libcurl. Standard environment variables such as
`HTTPS_PROXY`, `HTTP_PROXY`, `NO_PROXY`, `SSL_CERT_FILE`, and `CURL_CA_BUNDLE`
can be used where supported by the libcurl build.

## Vulnerabilities

Report suspected vulnerabilities using the process in the repository
`SECURITY.md`. Do not include API keys, provider credentials, private data, or
full model prompts in public issues.
