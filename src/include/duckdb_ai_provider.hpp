#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
class ClientContext;

namespace duckdb_ai {

//! Runtime provider configuration after aliases, defaults, environment variables, and explicit options are resolved.
struct ProviderConfig {
	std::string provider;
	std::string protocol;
	std::string default_model;
	std::string model;
	std::string base_url;
	std::string api_key_env;
	std::string api_key;
	bool requires_api_key;
};

//! Conservative provider limits used to plan embedding batches and expose runtime capabilities.
struct ProviderCapabilities {
	std::string provider;
	std::string protocol;
	int64_t max_batch_inputs;
	int64_t max_batch_tokens;
	int64_t max_request_bytes;
	int64_t context_tokens;
	int64_t embedding_dimensions;
	bool native_batch_jobs;
};

//! Per-call completion, embedding, retry, rate-limit, logging, and cost-estimation options.
struct CompletionOptions {
	std::string model;
	std::string provider;
	std::string secret_name;
	bool explicit_model = false;
	bool explicit_provider = false;
	bool explicit_base_url = false;
	std::string system_prompt;
	std::string base_url;
	std::string api_key;
	bool has_temperature = false;
	double temperature = 0;
	bool has_max_tokens = false;
	int64_t max_tokens = 0;
	bool has_timeout_seconds = false;
	int64_t timeout_seconds = 0;
	bool has_retry_count = false;
	int64_t retry_count = 0;
	bool has_retry_backoff_ms = false;
	int64_t retry_backoff_ms = 1000;
	bool has_max_concurrent_requests = false;
	int64_t max_concurrent_requests = 0;
	bool has_min_request_interval_ms = false;
	int64_t min_request_interval_ms = 0;
	bool has_token_limit_per_minute = false;
	int64_t token_limit_per_minute = 0;
	bool has_input_token_price_per_million = false;
	double input_token_price_per_million = 0;
	bool has_output_token_price_per_million = false;
	double output_token_price_per_million = 0;
	bool has_use_builtin_model_prices = false;
	bool use_builtin_model_prices = false;
	bool has_cache = false;
	bool cache = false;
	bool has_cache_ttl_seconds = false;
	int64_t cache_ttl_seconds = 0;
	bool has_response_cache_max_entries = false;
	int64_t response_cache_max_entries = 0;
	bool has_prompt_cache = false;
	bool prompt_cache = false;
	bool has_connect_timeout_seconds = false;
	int64_t connect_timeout_seconds = 0;
	std::string allowed_hosts;
	std::string log_endpoint;
	std::string log_format;
	std::string log_tags;
	bool has_log_include_text = false;
	bool log_include_text = false;
	bool has_log_strict = false;
	bool log_strict = false;
	bool has_log_sample_rate = false;
	double log_sample_rate = 1;
	bool fail_on_error = true;
	std::string on_error;
	std::string response_format;
	std::string response_schema;
	std::string function_name;
	std::string query_id;
	std::string operation_id;
	std::string parent_operation_id;
	std::string model_options;
	ClientContext *client_context = nullptr;
	void *runtime_state = nullptr;
};

//! Parsed result from a text-generation provider response.
struct CompletionResult {
	std::string text;
	std::string raw_response;
	long http_status;
	int64_t prompt_tokens;
	int64_t completion_tokens;
	int64_t total_tokens;
	int64_t cached_prompt_tokens;
	int64_t cache_creation_prompt_tokens;
	int64_t elapsed_ms;
	int64_t retries;
	bool cache_hit;
	std::string finish_reason;
};

//! Parsed result from an embedding provider response.
struct EmbeddingResult {
	std::vector<double> values;
	std::string raw_response;
	long http_status;
	int64_t prompt_tokens;
	int64_t total_tokens;
	int64_t elapsed_ms;
	int64_t retries;
	bool cache_hit;
};

//! In-process usage record exposed through ai_usage().
struct UsageEvent {
	uint64_t event_id;
	std::string created_at;
	std::string event;
	std::string function_name;
	std::string query_id;
	std::string operation_id;
	std::string parent_operation_id;
	std::string provider;
	std::string protocol;
	std::string model;
	int64_t prompt_chars;
	int64_t response_chars;
	int64_t input_chars;
	int64_t dimensions;
	int64_t prompt_tokens;
	int64_t completion_tokens;
	int64_t total_tokens;
	int64_t cached_prompt_tokens;
	int64_t cache_creation_prompt_tokens;
	int64_t elapsed_ms;
	int64_t retries;
	long http_status;
	bool cache_hit;
	std::string status;
	std::string error;
	double estimated_cost_usd;
};

//! Bounded-buffer statistics exposed through ai_usage_summary().
struct UsageBufferStats {
	uint64_t retained_events;
	uint64_t dropped_events;
	uint64_t queued_log_events;
	uint64_t dropped_log_events;
};

//! Built-in model pricing row exposed through ai_model_prices().
struct ModelPrice {
	std::string provider;
	std::string model;
	std::string operation;
	double input_token_price_per_million;
	double output_token_price_per_million;
	std::string source_url;
	std::string source_note;
	std::string last_reviewed;
};

//! A JSON Schema property projected by ai_complete_record().
struct JsonSchemaProperty {
	std::string name;
	std::string type;
	std::string item_type;
	bool required;
	std::vector<JsonSchemaProperty> children;
	std::vector<JsonSchemaProperty> item_children;
};

//! JSON value category used by ai_complete_record() projection.
enum class JsonExtractedKind : uint8_t { MISSING, NULL_VALUE, BOOLEAN, NUMBER, STRING, ARRAY, OBJECT };

//! JSON value extracted from an object field for typed DuckDB projection.
struct JsonExtractedValue {
	JsonExtractedKind kind = JsonExtractedKind::MISSING;
	bool boolean_value = false;
	double number_value = 0;
	bool number_is_integer = false;
	std::string string_value;
	std::vector<std::string> string_array;
	std::vector<JsonExtractedValue> array_values;
	std::vector<std::pair<std::string, JsonExtractedValue>> object_values;
	std::string json_value;
};

//! Resolve a provider/model pair using provider aliases, defaults, and environment overrides.
ProviderConfig ResolveProvider(const std::string &provider, const std::string &model);
//! Resolve provider configuration from a full option set.
ProviderConfig ResolveProvider(const CompletionOptions &options);
//! Snapshot non-secret environment defaults into options once for a query bind.
void SnapshotEnvironmentOptions(CompletionOptions &options);
//! Normalize provider aliases such as claude -> anthropic and local -> openai_compatible.
std::string NormalizeProviderName(const std::string &provider);
//! Return the default base URL for a supported provider.
std::string ProviderBaseUrl(const std::string &provider);
//! Return the provider protocol used for request/response shaping.
std::string ProviderProtocol(const std::string &provider);
//! Return conservative execution limits for the resolved provider/model.
ProviderCapabilities GetProviderCapabilities(const CompletionOptions &options);
//! Apply safe pricing metadata stored in an external model's JSON options.
void ApplyModelProfileOptions(CompletionOptions &options);
//! Return a local approximate token count used by ai_count_tokens() and token-aware pacing.
int64_t EstimateTokenCount(const std::string &input);
//! Build a completion request payload without making a network call.
std::string BuildRequestJson(const std::string &prompt, const std::string &model, const std::string &provider);
//! Build a completion request payload from a full option set.
std::string BuildRequestJson(const std::string &prompt, const CompletionOptions &options);
//! Execute a completion request and parse the provider response.
CompletionResult Complete(const std::string &prompt, const std::string &model, const std::string &provider);
//! Execute a completion request from a full option set.
CompletionResult Complete(const std::string &prompt, const CompletionOptions &options);
//! Execute a dedicated PII redaction request from a full option set.
CompletionResult Redact(const std::string &text, const CompletionOptions &options);
//! Build an embedding request payload without making a network call.
std::string BuildEmbeddingRequestJson(const std::string &input, const std::string &model, const std::string &provider);
//! Build an embedding request payload from a full option set.
std::string BuildEmbeddingRequestJson(const std::string &input, const CompletionOptions &options);
//! Execute an embedding request and parse the provider response.
EmbeddingResult Embed(const std::string &input, const std::string &model, const std::string &provider);
//! Execute an embedding request from a full option set.
EmbeddingResult Embed(const std::string &input, const CompletionOptions &options);
//! Execute a batched embedding request from a full option set.
std::vector<EmbeddingResult> EmbedMany(const std::vector<std::string> &inputs, const CompletionOptions &options);
//! Record an embedding served by the query-local similarity cache.
void RecordEmbeddingCacheHit(const std::string &input, int64_t dimensions, const CompletionOptions &options);
//! POST a JSON control-plane request to DUCKDB_AI_CONTROL_PLANE_URL.
std::string ControlPlaneRequest(const std::string &path, const std::string &payload, const CompletionOptions &options);
//! Return the effective max_concurrent_requests value from options or env.
int64_t EffectiveMaxConcurrentRequests(const CompletionOptions &options);
//! Initialize provider runtime dependencies such as libcurl.
void InitializeProviderRuntime();
//! Attach per-database provider runtime state before provider calls run on worker threads.
void AttachProviderRuntimeState(CompletionOptions &options, ClientContext &context);
//! Return a snapshot of the bounded in-process usage buffer.
std::vector<UsageEvent> UsageEvents();
//! Return a snapshot of the bounded per-database usage buffer.
std::vector<UsageEvent> UsageEvents(ClientContext &context);
//! Return bounded usage/log buffer statistics.
UsageBufferStats UsageStats();
//! Return per-database bounded usage/log buffer statistics.
UsageBufferStats UsageStats(ClientContext &context);
//! Clear the in-process usage buffer.
void ClearUsageEvents();
//! Clear the per-database usage buffer.
void ClearUsageEvents(ClientContext &context);
//! Clear the in-process response cache.
void ClearResponseCache();
//! Clear the per-database response cache.
void ClearResponseCache(ClientContext &context);
//! Record local deterministic work that does not call a provider, such as ai_schema_prompt().
void RecordLocalUsageEvent(const std::string &event, int64_t input_chars, int64_t response_chars);
//! Record local deterministic work against a specific client context.
void RecordLocalUsageEvent(ClientContext *context, const std::string &event, int64_t input_chars,
                           int64_t response_chars);
//! Return the built-in model pricing catalog.
std::vector<ModelPrice> ModelPrices();
//! Validate that the input is one complete JSON document.
bool ValidateJsonDocument(const std::string &input, std::string &error);
//! Validate a JSON document against the supported JSON Schema subset.
bool ValidateJsonAgainstSchema(const std::string &input, const std::string &schema, std::string &error);
//! Extract top-level object properties from a JSON Schema object.
bool ExtractJsonSchemaProperties(const std::string &schema, std::vector<JsonSchemaProperty> &properties,
                                 std::string &error);
//! Extract named fields from a top-level JSON object for typed projection.
bool ExtractJsonObjectFields(const std::string &input, const std::vector<std::string> &field_names,
                             std::vector<JsonExtractedValue> &values, std::string &error);
//! Extract strings from a top-level JSON array.
bool ExtractJsonStringArray(const std::string &input, std::vector<std::string> &values, std::string &error);

} // namespace duckdb_ai
} // namespace duckdb
