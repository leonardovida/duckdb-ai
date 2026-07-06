|                   name                   |                                                       description                                                        | input_type | scope  | aliases |
|------------------------------------------|--------------------------------------------------------------------------------------------------------------------------|------------|--------|---------|
| duckdb_ai_aggregate_model                | Default AI model for aggregate functions                                                                                 | VARCHAR    | GLOBAL | []      |
| duckdb_ai_allowed_hosts                  | Comma-separated AI provider host allowlist for duckdb_ai; empty allows all hosts                                         | VARCHAR    | GLOBAL | []      |
| duckdb_ai_base_url                       | Default AI provider base URL override for duckdb_ai                                                                      | VARCHAR    | GLOBAL | []      |
| duckdb_ai_cache                          | Cache successful AI provider responses in the current DuckDB instance                                                    | BOOLEAN    | GLOBAL | []      |
| duckdb_ai_cache_max_entries              | Maximum response-cache entries between 0 and 1000000; 0 disables response-cache storage, -1 uses default                 | BIGINT     | GLOBAL | []      |
| duckdb_ai_cache_ttl_seconds              | Maximum response-cache entry age in seconds between 0 and 31536000; 0 disables age expiry, -1 uses default               | BIGINT     | GLOBAL | []      |
| duckdb_ai_completion_model               | Default AI model for completion functions                                                                                | VARCHAR    | GLOBAL | []      |
| duckdb_ai_connect_timeout_seconds        | AI provider connection timeout in seconds between 1 and 31536000; -1 uses default                                        | BIGINT     | GLOBAL | []      |
| duckdb_ai_embedding_model                | Default AI model for embedding functions                                                                                 | VARCHAR    | GLOBAL | []      |
| duckdb_ai_input_token_price_per_million  | Input token price per million tokens for estimated AI usage cost; -1 disables cost estimates                             | DOUBLE     | GLOBAL | []      |
| duckdb_ai_log_endpoint                   | HTTP endpoint for privacy-minimized AI usage logs                                                                        | VARCHAR    | GLOBAL | []      |
| duckdb_ai_log_format                     | AI usage log payload format: generic_json or otlp_json                                                                   | VARCHAR    | GLOBAL | []      |
| duckdb_ai_log_include_text               | Include prompt and response text in AI usage logs                                                                        | BOOLEAN    | GLOBAL | []      |
| duckdb_ai_log_sample_rate                | AI usage log sampling rate between 0 and 1; -1 uses default                                                              | DOUBLE     | GLOBAL | []      |
| duckdb_ai_log_strict                     | Fail SQL queries when AI usage log delivery fails                                                                        | BOOLEAN    | GLOBAL | []      |
| duckdb_ai_log_tags                       | Optional tag string included in AI usage logs                                                                            | VARCHAR    | GLOBAL | []      |
| duckdb_ai_max_concurrent_requests        | Maximum concurrent AI provider requests between 0 and 64; 0 disables the limit, -1 uses default                          | BIGINT     | GLOBAL | []      |
| duckdb_ai_min_request_interval_ms        | Minimum milliseconds between AI provider request starts between 0 and 60000; -1 uses default                             | BIGINT     | GLOBAL | []      |
| duckdb_ai_model                          | Default AI model for duckdb_ai                                                                                           | VARCHAR    | GLOBAL | []      |
| duckdb_ai_on_error                       | AI error handling: fail, null, or capture                                                                                | VARCHAR    | GLOBAL | []      |
| duckdb_ai_output_token_price_per_million | Output token price per million tokens for estimated AI usage cost; -1 disables cost estimates                            | DOUBLE     | GLOBAL | []      |
| duckdb_ai_prompt_cache                   | Enable provider-side prompt caching hints when supported                                                                 | BOOLEAN    | GLOBAL | []      |
| duckdb_ai_provider                       | Default AI provider for duckdb_ai                                                                                        | VARCHAR    | GLOBAL | []      |
| duckdb_ai_response_format                | Default AI response format: text, json_object, or json_schema                                                            | VARCHAR    | GLOBAL | []      |
| duckdb_ai_response_schema                | Default AI JSON schema object for structured responses                                                                   | VARCHAR    | GLOBAL | []      |
| duckdb_ai_retry_backoff_ms               | AI provider retry backoff in milliseconds between 0 and 60000; -1 uses default                                           | BIGINT     | GLOBAL | []      |
| duckdb_ai_retry_count                    | AI provider retry count between 0 and 10; -1 uses default                                                                | BIGINT     | GLOBAL | []      |
| duckdb_ai_sql_assistant_model            | Default AI model for SQL assistant functions                                                                             | VARCHAR    | GLOBAL | []      |
| duckdb_ai_task_model                     | Default AI model for text task functions                                                                                 | VARCHAR    | GLOBAL | []      |
| duckdb_ai_timeout_seconds                | AI provider HTTP timeout in seconds; 0 uses the extension default                                                        | BIGINT     | GLOBAL | []      |
| duckdb_ai_token_limit_per_minute         | Maximum estimated AI provider tokens per rolling minute between 0 and 10000000000; 0 disables the limit, -1 uses default | BIGINT     | GLOBAL | []      |
| duckdb_ai_use_builtin_model_prices       | Use duckdb_ai built-in model price catalog for estimated AI usage cost                                                   | BOOLEAN    | GLOBAL | []      |
