#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
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

//! Per-call completion, embedding, retry, rate-limit, logging, and cost-estimation options.
struct CompletionOptions {
	std::string model;
	std::string provider;
	std::string secret_name;
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
	bool has_input_token_price_per_million = false;
	double input_token_price_per_million = 0;
	bool has_output_token_price_per_million = false;
	double output_token_price_per_million = 0;
	bool has_use_builtin_model_prices = false;
	bool use_builtin_model_prices = false;
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
	std::string response_format;
	std::string response_schema;
};

//! Parsed result from a text-generation provider response.
struct CompletionResult {
	std::string text;
	std::string raw_response;
	long http_status;
	int64_t prompt_tokens;
	int64_t completion_tokens;
	int64_t total_tokens;
	int64_t elapsed_ms;
};

//! Parsed result from an embedding provider response.
struct EmbeddingResult {
	std::vector<double> values;
	std::string raw_response;
	long http_status;
	int64_t prompt_tokens;
	int64_t total_tokens;
	int64_t elapsed_ms;
};

//! In-process usage record exposed through ai_usage().
struct UsageEvent {
	uint64_t event_id;
	std::string created_at;
	std::string event;
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
	int64_t elapsed_ms;
	long http_status;
	double estimated_cost_usd;
};

//! Built-in model pricing row exposed through ai_models().
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
//! Normalize provider aliases such as anthropic -> claude and local -> openai_compatible.
std::string NormalizeProviderName(const std::string &provider);
//! Return the default base URL for a supported provider.
std::string ProviderBaseUrl(const std::string &provider);
//! Return the provider protocol used for request/response shaping.
std::string ProviderProtocol(const std::string &provider);
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
//! Return a snapshot of the bounded in-process usage buffer.
std::vector<UsageEvent> UsageEvents();
//! Clear the in-process usage buffer.
void ClearUsageEvents();
//! Record local deterministic work that does not call a provider, such as ai_schema_prompt().
void RecordLocalUsageEvent(const std::string &event, int64_t input_chars, int64_t response_chars);
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

} // namespace duckdb_ai
} // namespace duckdb
