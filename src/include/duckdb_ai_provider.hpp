#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace duckdb_ai {

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

struct CompletionResult {
	std::string text;
	std::string raw_response;
	long http_status;
	int64_t prompt_tokens;
	int64_t completion_tokens;
	int64_t total_tokens;
	int64_t elapsed_ms;
};

struct EmbeddingResult {
	std::vector<double> values;
	std::string raw_response;
	long http_status;
	int64_t prompt_tokens;
	int64_t total_tokens;
	int64_t elapsed_ms;
};

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

struct JsonSchemaProperty {
	std::string name;
	std::string type;
	std::string item_type;
	bool required;
	std::vector<JsonSchemaProperty> children;
	std::vector<JsonSchemaProperty> item_children;
};

enum class JsonExtractedKind : uint8_t { MISSING, NULL_VALUE, BOOLEAN, NUMBER, STRING, ARRAY, OBJECT };

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

ProviderConfig ResolveProvider(const std::string &provider, const std::string &model);
ProviderConfig ResolveProvider(const CompletionOptions &options);
std::string NormalizeProviderName(const std::string &provider);
std::string ProviderBaseUrl(const std::string &provider);
std::string ProviderProtocol(const std::string &provider);
std::string BuildRequestJson(const std::string &prompt, const std::string &model, const std::string &provider);
std::string BuildRequestJson(const std::string &prompt, const CompletionOptions &options);
CompletionResult Complete(const std::string &prompt, const std::string &model, const std::string &provider);
CompletionResult Complete(const std::string &prompt, const CompletionOptions &options);
std::string BuildEmbeddingRequestJson(const std::string &input, const std::string &model, const std::string &provider);
std::string BuildEmbeddingRequestJson(const std::string &input, const CompletionOptions &options);
EmbeddingResult Embed(const std::string &input, const std::string &model, const std::string &provider);
EmbeddingResult Embed(const std::string &input, const CompletionOptions &options);
std::vector<UsageEvent> UsageEvents();
void ClearUsageEvents();
void RecordLocalUsageEvent(const std::string &event, int64_t input_chars, int64_t response_chars);
std::vector<ModelPrice> ModelPrices();
bool ValidateJsonDocument(const std::string &input, std::string &error);
bool ValidateJsonAgainstSchema(const std::string &input, const std::string &schema, std::string &error);
bool ExtractJsonSchemaProperties(const std::string &schema, std::vector<JsonSchemaProperty> &properties,
                                 std::string &error);
bool ExtractJsonObjectFields(const std::string &input, const std::vector<std::string> &field_names,
                             std::vector<JsonExtractedValue> &values, std::string &error);

} // namespace duckdb_ai
} // namespace duckdb
