#define DUCKDB_EXTENSION_MAIN

#include "duckdb_ai_extension.hpp"
#include "duckdb_ai_provider.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <set>
#include <sstream>

namespace duckdb {

namespace {

bool CompletionOptionsEqual(const duckdb_ai::CompletionOptions &left, const duckdb_ai::CompletionOptions &right) {
	return left.model == right.model && left.provider == right.provider && left.secret_name == right.secret_name &&
	       left.system_prompt == right.system_prompt && left.base_url == right.base_url &&
	       left.api_key == right.api_key && left.has_temperature == right.has_temperature &&
	       left.temperature == right.temperature && left.has_max_tokens == right.has_max_tokens &&
	       left.max_tokens == right.max_tokens && left.has_timeout_seconds == right.has_timeout_seconds &&
	       left.timeout_seconds == right.timeout_seconds && left.has_retry_count == right.has_retry_count &&
	       left.retry_count == right.retry_count && left.has_retry_backoff_ms == right.has_retry_backoff_ms &&
	       left.retry_backoff_ms == right.retry_backoff_ms &&
	       left.has_max_concurrent_requests == right.has_max_concurrent_requests &&
	       left.max_concurrent_requests == right.max_concurrent_requests &&
	       left.has_min_request_interval_ms == right.has_min_request_interval_ms &&
	       left.min_request_interval_ms == right.min_request_interval_ms &&
	       left.has_input_token_price_per_million == right.has_input_token_price_per_million &&
	       left.input_token_price_per_million == right.input_token_price_per_million &&
	       left.has_output_token_price_per_million == right.has_output_token_price_per_million &&
	       left.output_token_price_per_million == right.output_token_price_per_million &&
	       left.has_use_builtin_model_prices == right.has_use_builtin_model_prices &&
	       left.use_builtin_model_prices == right.use_builtin_model_prices && left.log_endpoint == right.log_endpoint &&
	       left.log_format == right.log_format && left.log_tags == right.log_tags &&
	       left.has_log_include_text == right.has_log_include_text && left.log_include_text == right.log_include_text &&
	       left.has_log_strict == right.has_log_strict && left.log_strict == right.log_strict &&
	       left.has_log_sample_rate == right.has_log_sample_rate && left.log_sample_rate == right.log_sample_rate &&
	       left.fail_on_error == right.fail_on_error && left.response_format == right.response_format &&
	       left.response_schema == right.response_schema;
}

struct AiUsageScanData : public GlobalTableFunctionState {
	AiUsageScanData() : offset(0), events(duckdb_ai::UsageEvents()) {
	}

	idx_t offset;
	vector<duckdb_ai::UsageEvent> events;
};

struct AiClearUsageScanData : public GlobalTableFunctionState {
	AiClearUsageScanData() : emitted(false) {
	}

	bool emitted;
};

struct AiModelPricesScanData : public GlobalTableFunctionState {
	AiModelPricesScanData() : offset(0), prices(duckdb_ai::ModelPrices()) {
	}

	idx_t offset;
	vector<duckdb_ai::ModelPrice> prices;
};

struct AiSecretsScanData : public GlobalTableFunctionState {
	idx_t offset = 0;
	vector<SecretEntry> secrets;
};

struct PromptSchemaOptions {
	vector<std::string> include_tables;
	vector<std::string> exclude_tables;
	int64_t sample_rows = 0;
};

struct PromptSchemaBindData : public FunctionData {
	explicit PromptSchemaBindData(PromptSchemaOptions options_p) : options(std::move(options_p)) {
	}

	PromptSchemaOptions options;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<PromptSchemaBindData>(options);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<PromptSchemaBindData>();
		return options.include_tables == other.options.include_tables &&
		       options.exclude_tables == other.options.exclude_tables &&
		       options.sample_rows == other.options.sample_rows;
	}
};

struct PromptSchemaScanData : public GlobalTableFunctionState {
	PromptSchemaScanData() : emitted(false) {
	}

	bool emitted;
};

enum class PromptAssistantKind : uint8_t { EXPLAIN, FIXUP, FIX_LINE };

struct PromptAssistantBindData : public FunctionData {
	explicit PromptAssistantBindData(PromptAssistantKind kind_p) : kind(kind_p) {
	}

	PromptAssistantKind kind;
	duckdb_ai::CompletionOptions options;
	std::string sql;
	std::string error_message;
	std::string schema_context;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<PromptAssistantBindData>(kind);
		result->options = options;
		result->sql = sql;
		result->error_message = error_message;
		result->schema_context = schema_context;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<PromptAssistantBindData>();
		return kind == other.kind && CompletionOptionsEqual(options, other.options) && sql == other.sql &&
		       error_message == other.error_message && schema_context == other.schema_context;
	}
};

struct PromptAssistantScanData : public GlobalTableFunctionState {
	PromptAssistantScanData() : emitted(false) {
	}

	bool emitted;
};

struct SchemaColumnInfo {
	std::string name;
	std::string type;
	bool nullable;
	std::string default_value;
	std::string comment;
};

struct SchemaSampleRow {
	vector<std::pair<std::string, std::string>> values;
};

struct SchemaTableInfo {
	std::string catalog;
	std::string schema;
	std::string table;
	std::string comment;
	std::string sql;
	int64_t estimated_rows;
	vector<SchemaColumnInfo> columns;
	vector<SchemaSampleRow> sample_rows;
	std::string sample_error;
};

vector<std::string> ReadIncludeTablesValue(const Value &value_p, const std::string &name,
                                           const std::string &function_name);
int64_t ReadSampleRowsValue(const Value &value_p, const std::string &function_name);

struct AiCompletionBindData : public FunctionData {
	explicit AiCompletionBindData(bool request_json_p, bool validate_json_output_p = false)
	    : request_json(request_json_p), validate_json_output(validate_json_output_p) {
	}

	bool request_json;
	bool validate_json_output;
	duckdb_ai::CompletionOptions options;
	idx_t prompt_index = 0;
	bool has_model_arg = false;
	idx_t model_index = 0;
	bool has_provider_arg = false;
	idx_t provider_index = 0;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<AiCompletionBindData>(request_json, validate_json_output);
		result->options = options;
		result->prompt_index = prompt_index;
		result->has_model_arg = has_model_arg;
		result->model_index = model_index;
		result->has_provider_arg = has_provider_arg;
		result->provider_index = provider_index;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AiCompletionBindData>();
		return request_json == other.request_json && validate_json_output == other.validate_json_output &&
		       CompletionOptionsEqual(options, other.options) && prompt_index == other.prompt_index &&
		       has_model_arg == other.has_model_arg && model_index == other.model_index &&
		       has_provider_arg == other.has_provider_arg && provider_index == other.provider_index;
	}
};

struct AiRecordColumn {
	std::string name;
	LogicalType type;
	std::string schema_type;
	std::string item_schema_type;
	vector<AiRecordColumn> children;
	vector<AiRecordColumn> item_children;
};

bool AiRecordColumnsEqual(const vector<AiRecordColumn> &left, const vector<AiRecordColumn> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (idx_t i = 0; i < left.size(); i++) {
		if (left[i].name != right[i].name || left[i].type != right[i].type ||
		    left[i].schema_type != right[i].schema_type || left[i].item_schema_type != right[i].item_schema_type ||
		    !AiRecordColumnsEqual(left[i].children, right[i].children) ||
		    !AiRecordColumnsEqual(left[i].item_children, right[i].item_children)) {
			return false;
		}
	}
	return true;
}

struct AiRecordBindData : public FunctionData {
	std::string prompt;
	std::string response_schema;
	duckdb_ai::CompletionOptions options;
	vector<AiRecordColumn> columns;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<AiRecordBindData>();
		result->prompt = prompt;
		result->response_schema = response_schema;
		result->options = options;
		result->columns = columns;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AiRecordBindData>();
		return prompt == other.prompt && response_schema == other.response_schema &&
		       CompletionOptionsEqual(options, other.options) && AiRecordColumnsEqual(columns, other.columns);
	}
};

struct AiRecordScanData : public GlobalTableFunctionState {
	AiRecordScanData() : emitted(false) {
	}

	bool emitted;
};

enum class AiTaskKind : uint8_t { SUMMARIZE, SENTIMENT, FIX_GRAMMAR, MASK, TRANSLATE, CLASSIFY, EXTRACT, FILTER };

enum class AiAggregateKind : uint8_t { GENERIC, SUMMARIZE };

struct AiTaskBindData : public FunctionData {
	AiTaskBindData(AiTaskKind task_p, idx_t required_args_p) : task(task_p), required_args(required_args_p) {
	}

	AiTaskKind task;
	idx_t required_args;
	duckdb_ai::CompletionOptions options;
	bool has_model_arg = false;
	idx_t model_index = 0;
	bool has_provider_arg = false;
	idx_t provider_index = 0;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<AiTaskBindData>(task, required_args);
		result->options = options;
		result->has_model_arg = has_model_arg;
		result->model_index = model_index;
		result->has_provider_arg = has_provider_arg;
		result->provider_index = provider_index;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AiTaskBindData>();
		return task == other.task && required_args == other.required_args &&
		       CompletionOptionsEqual(options, other.options) && has_model_arg == other.has_model_arg &&
		       model_index == other.model_index && has_provider_arg == other.has_provider_arg &&
		       provider_index == other.provider_index;
	}
};

struct AiAggregateBindData : public FunctionData {
	explicit AiAggregateBindData(AiAggregateKind kind_p) : kind(kind_p) {
	}

	AiAggregateKind kind;
	duckdb_ai::CompletionOptions options;
	std::string instruction;
	std::string separator = "\n---\n";
	idx_t max_context_chars = 100000;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<AiAggregateBindData>(kind);
		result->options = options;
		result->instruction = instruction;
		result->separator = separator;
		result->max_context_chars = max_context_chars;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AiAggregateBindData>();
		return kind == other.kind && CompletionOptionsEqual(options, other.options) &&
		       instruction == other.instruction && separator == other.separator &&
		       max_context_chars == other.max_context_chars;
	}
};

struct AiAggregateState {
	idx_t size;
	idx_t alloc_size;
	idx_t row_count;
	bool truncated;
	char *dataptr;
};

struct AiPromptSqlBindData : public FunctionData {
	duckdb_ai::CompletionOptions options;
	std::string schema_context;
	PromptSchemaOptions schema_options;
	bool has_schema_context_arg = false;
	idx_t schema_context_index = 0;
	bool has_model_arg = false;
	idx_t model_index = 0;
	bool has_provider_arg = false;
	idx_t provider_index = 0;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<AiPromptSqlBindData>();
		result->options = options;
		result->schema_context = schema_context;
		result->schema_options = schema_options;
		result->has_schema_context_arg = has_schema_context_arg;
		result->schema_context_index = schema_context_index;
		result->has_model_arg = has_model_arg;
		result->model_index = model_index;
		result->has_provider_arg = has_provider_arg;
		result->provider_index = provider_index;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AiPromptSqlBindData>();
		return CompletionOptionsEqual(options, other.options) && schema_context == other.schema_context &&
		       schema_options.include_tables == other.schema_options.include_tables &&
		       schema_options.exclude_tables == other.schema_options.exclude_tables &&
		       schema_options.sample_rows == other.schema_options.sample_rows &&
		       has_schema_context_arg == other.has_schema_context_arg &&
		       schema_context_index == other.schema_context_index && has_model_arg == other.has_model_arg &&
		       model_index == other.model_index && has_provider_arg == other.has_provider_arg &&
		       provider_index == other.provider_index;
	}
};

std::string LowerAscii(std::string input) {
	std::transform(input.begin(), input.end(), input.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return input;
}

std::string NormalizeResponseFormatValue(const std::string &value, const std::string &function_name) {
	auto format = LowerAscii(value);
	if (format.empty() || format == "text" || format == "json_object" || format == "json_schema") {
		return format;
	}
	throw BinderException("%s option \"response_format\" must be one of: text, json_object, json_schema",
	                      function_name);
}

void ValidateResponseSchemaValue(const std::string &schema, const std::string &function_name) {
	std::string error;
	if (!duckdb_ai::ValidateJsonDocument(schema, error)) {
		throw BinderException("%s option \"response_schema\" must be valid JSON: %s", function_name, error);
	}
	auto first = schema.size();
	for (idx_t i = 0; i < schema.size(); i++) {
		if (!std::isspace(static_cast<unsigned char>(schema[i]))) {
			first = i;
			break;
		}
	}
	if (first == schema.size() || schema[first] != '{') {
		throw BinderException("%s option \"response_schema\" must be a JSON object", function_name);
	}
}

bool TryGetOptionType(const std::string &name, LogicalType &type) {
	if (name == "model" || name == "provider" || name == "profile" || name == "secret" || name == "secret_name" ||
	    name == "system_prompt" || name == "base_url" || name == "response_format" || name == "response_schema" ||
	    name == "json_schema" || name == "log_format" || name == "log_tags") {
		type = LogicalType::VARCHAR;
		return true;
	}
	if (name == "temperature" || name == "log_sample_rate" || name == "input_token_price_per_million" ||
	    name == "output_token_price_per_million") {
		type = LogicalType::DOUBLE;
		return true;
	}
	if (name == "max_tokens" || name == "retry_count" || name == "retry_backoff_ms" ||
	    name == "max_concurrent_requests" || name == "min_request_interval_ms") {
		type = LogicalType::BIGINT;
		return true;
	}
	if (name == "timeout_seconds") {
		type = LogicalType::BIGINT;
		return true;
	}
	if (name == "fail_on_error" || name == "use_builtin_model_prices") {
		type = LogicalType::BOOLEAN;
		return true;
	}
	return false;
}

LogicalType OptionType(const std::string &name) {
	LogicalType type;
	if (TryGetOptionType(name, type)) {
		return type;
	}
	throw BinderException("Unsupported AI option \"%s\". Supported options: model, provider, temperature, "
	                      "system_prompt, max_tokens, base_url, timeout_seconds, retry_count, retry_backoff_ms, "
	                      "max_concurrent_requests, min_request_interval_ms, input_token_price_per_million, "
	                      "output_token_price_per_million, use_builtin_model_prices, fail_on_error, profile, secret, "
	                      "response_format, response_schema, json_schema, log_format, log_tags, log_sample_rate",
	                      name);
}

LogicalType PromptSqlOptionType(const std::string &name) {
	if (name == "schema_context" || name == "schema") {
		return LogicalType::VARCHAR;
	}
	if (name == "include_tables" || name == "exclude_tables") {
		return LogicalType::LIST(LogicalType::VARCHAR);
	}
	if (name == "sample_rows") {
		return LogicalType::BIGINT;
	}
	return OptionType(name);
}

Value EvaluateConstantOption(ClientContext &context, Expression &expr, const std::string &name,
                             const LogicalType &target_type) {
	if (!expr.IsFoldable()) {
		throw BinderException("AI option \"%s\" must be a constant expression", name);
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, expr).DefaultCastAs(target_type);
	if (value.IsNull()) {
		throw BinderException("AI option \"%s\" must not be NULL", name);
	}
	return value;
}

Value CastOptionValue(const Value &value_p, const std::string &function_name, const std::string &name,
                      const LogicalType &target_type) {
	if (value_p.IsNull()) {
		throw BinderException("%s option \"%s\" must not be NULL", function_name, name);
	}
	return value_p.DefaultCastAs(target_type);
}

std::string OptionStringValue(const Value &value, const std::string &function_name, const std::string &name) {
	return StringValue::Get(CastOptionValue(value, function_name, name, LogicalType::VARCHAR));
}

double OptionDoubleValue(const Value &value, const std::string &function_name, const std::string &name) {
	return DoubleValue::Get(CastOptionValue(value, function_name, name, LogicalType::DOUBLE));
}

int64_t OptionBigIntValue(const Value &value, const std::string &function_name, const std::string &name) {
	return BigIntValue::Get(CastOptionValue(value, function_name, name, LogicalType::BIGINT));
}

bool OptionBoolValue(const Value &value, const std::string &function_name, const std::string &name) {
	return BooleanValue::Get(CastOptionValue(value, function_name, name, LogicalType::BOOLEAN));
}

std::string NormalizeLogFormatValue(const std::string &value, const std::string &function_name) {
	auto log_format = LowerAscii(value);
	if (log_format == "generic_json" || log_format == "generic" || log_format == "json" || log_format == "otlp_json" ||
	    log_format == "otlp") {
		return log_format;
	}
	throw BinderException("%s option \"log_format\" must be one of: generic_json, otlp_json", function_name);
}

bool ApplyCompletionValueOption(duckdb_ai::CompletionOptions &options, const std::string &function_name,
                                const std::string &name, const Value &value, bool allow_response_options,
                                bool allow_log_payload_options) {
	if (name == "model") {
		options.model = OptionStringValue(value, function_name, name);
		return true;
	}
	if (name == "provider") {
		options.provider = OptionStringValue(value, function_name, name);
		return true;
	}
	if (name == "profile" || name == "secret" || name == "secret_name") {
		options.secret_name = OptionStringValue(value, function_name, name);
		return true;
	}
	if (name == "system_prompt") {
		options.system_prompt = OptionStringValue(value, function_name, name);
		return true;
	}
	if (name == "base_url") {
		options.base_url = OptionStringValue(value, function_name, name);
		return true;
	}
	if (name == "log_tags") {
		options.log_tags = OptionStringValue(value, function_name, name);
		return true;
	}
	if (name == "log_format") {
		options.log_format = NormalizeLogFormatValue(OptionStringValue(value, function_name, name), function_name);
		return true;
	}
	if (name == "response_format") {
		if (!allow_response_options) {
			return false;
		}
		options.response_format =
		    NormalizeResponseFormatValue(OptionStringValue(value, function_name, name), function_name);
		return true;
	}
	if (name == "response_schema" || name == "json_schema") {
		if (!allow_response_options) {
			return false;
		}
		options.response_schema = OptionStringValue(value, function_name, name);
		ValidateResponseSchemaValue(options.response_schema, function_name);
		return true;
	}
	if (name == "temperature") {
		options.temperature = OptionDoubleValue(value, function_name, name);
		if (options.temperature < 0 || options.temperature > 2) {
			throw BinderException("%s option \"temperature\" must be between 0 and 2", function_name);
		}
		options.has_temperature = true;
		return true;
	}
	if (name == "log_sample_rate") {
		options.log_sample_rate = OptionDoubleValue(value, function_name, name);
		if (options.log_sample_rate < 0 || options.log_sample_rate > 1) {
			throw BinderException("%s option \"log_sample_rate\" must be between 0 and 1", function_name);
		}
		options.has_log_sample_rate = true;
		return true;
	}
	if (name == "input_token_price_per_million") {
		options.input_token_price_per_million = OptionDoubleValue(value, function_name, name);
		if (!std::isfinite(options.input_token_price_per_million) || options.input_token_price_per_million < 0) {
			throw BinderException("%s option \"input_token_price_per_million\" must be greater than or equal to 0",
			                      function_name);
		}
		options.has_input_token_price_per_million = true;
		return true;
	}
	if (name == "output_token_price_per_million") {
		options.output_token_price_per_million = OptionDoubleValue(value, function_name, name);
		if (!std::isfinite(options.output_token_price_per_million) || options.output_token_price_per_million < 0) {
			throw BinderException("%s option \"output_token_price_per_million\" must be greater than or equal to 0",
			                      function_name);
		}
		options.has_output_token_price_per_million = true;
		return true;
	}
	if (name == "use_builtin_model_prices") {
		options.use_builtin_model_prices = OptionBoolValue(value, function_name, name);
		options.has_use_builtin_model_prices = true;
		return true;
	}
	if (name == "max_tokens") {
		options.max_tokens = OptionBigIntValue(value, function_name, name);
		if (options.max_tokens <= 0) {
			throw BinderException("%s option \"max_tokens\" must be greater than 0", function_name);
		}
		options.has_max_tokens = true;
		return true;
	}
	if (name == "retry_count") {
		options.retry_count = OptionBigIntValue(value, function_name, name);
		if (options.retry_count < 0 || options.retry_count > 10) {
			throw BinderException("%s option \"retry_count\" must be between 0 and 10", function_name);
		}
		options.has_retry_count = true;
		return true;
	}
	if (name == "retry_backoff_ms") {
		options.retry_backoff_ms = OptionBigIntValue(value, function_name, name);
		if (options.retry_backoff_ms < 0 || options.retry_backoff_ms > 60000) {
			throw BinderException("%s option \"retry_backoff_ms\" must be between 0 and 60000", function_name);
		}
		options.has_retry_backoff_ms = true;
		return true;
	}
	if (name == "max_concurrent_requests") {
		options.max_concurrent_requests = OptionBigIntValue(value, function_name, name);
		if (options.max_concurrent_requests < 0 || options.max_concurrent_requests > 1024) {
			throw BinderException("%s option \"max_concurrent_requests\" must be between 0 and 1024", function_name);
		}
		options.has_max_concurrent_requests = true;
		return true;
	}
	if (name == "min_request_interval_ms") {
		options.min_request_interval_ms = OptionBigIntValue(value, function_name, name);
		if (options.min_request_interval_ms < 0 || options.min_request_interval_ms > 60000) {
			throw BinderException("%s option \"min_request_interval_ms\" must be between 0 and 60000", function_name);
		}
		options.has_min_request_interval_ms = true;
		return true;
	}
	if (name == "timeout_seconds") {
		options.timeout_seconds = OptionBigIntValue(value, function_name, name);
		if (options.timeout_seconds <= 0) {
			throw BinderException("%s option \"timeout_seconds\" must be greater than 0", function_name);
		}
		options.has_timeout_seconds = true;
		return true;
	}
	if (name == "fail_on_error") {
		options.fail_on_error = OptionBoolValue(value, function_name, name);
		return true;
	}
	if (name == "log_include_text") {
		if (!allow_log_payload_options) {
			return false;
		}
		options.log_include_text = OptionBoolValue(value, function_name, name);
		options.has_log_include_text = true;
		return true;
	}
	if (name == "log_strict") {
		if (!allow_log_payload_options) {
			return false;
		}
		options.log_strict = OptionBoolValue(value, function_name, name);
		options.has_log_strict = true;
		return true;
	}
	return false;
}

void ApplyNamedOption(ClientContext &context, duckdb_ai::CompletionOptions &options, Expression &expr,
                      const std::string &name) {
	auto target_type = OptionType(name);
	auto value = EvaluateConstantOption(context, expr, name, target_type);
	if (!ApplyCompletionValueOption(options, "AI", name, value, true, false)) {
		throw InternalException("Unhandled AI option \"%s\"", name);
	}
}

void ApplyAggregateOption(ClientContext &context, AiAggregateBindData &bind_data, Expression &expr,
                          const std::string &name) {
	if (name == "instruction" || name == "task") {
		auto value = EvaluateConstantOption(context, expr, name, LogicalType::VARCHAR);
		bind_data.instruction = StringValue::Get(value);
		return;
	}
	if (name == "separator") {
		auto value = EvaluateConstantOption(context, expr, name, LogicalType::VARCHAR);
		bind_data.separator = StringValue::Get(value);
		return;
	}
	if (name == "max_context_chars") {
		auto value = EvaluateConstantOption(context, expr, name, LogicalType::BIGINT);
		auto max_context_chars = BigIntValue::Get(value);
		if (max_context_chars <= 0) {
			throw BinderException("AI aggregate option \"max_context_chars\" must be greater than 0");
		}
		bind_data.max_context_chars = NumericCast<idx_t>(max_context_chars);
		return;
	}
	ApplyNamedOption(context, bind_data.options, expr, name);
}

bool TryGetSetting(ClientContext &context, const std::string &name, Value &value) {
	return context.TryGetCurrentSetting(name, value) && !value.IsNull();
}

void ApplyStringSetting(ClientContext &context, const std::string &name, std::string &target) {
	Value value;
	if (!TryGetSetting(context, name, value)) {
		return;
	}
	auto setting = StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
	if (!setting.empty()) {
		target = setting;
	}
}

void ApplyBoolSetting(ClientContext &context, const std::string &name, bool &target, bool &has_target) {
	Value value;
	if (!TryGetSetting(context, name, value)) {
		return;
	}
	target = BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
	has_target = true;
}

void ApplyDoubleSetting(ClientContext &context, const std::string &name, double &target, bool &has_target) {
	Value value;
	if (!TryGetSetting(context, name, value)) {
		return;
	}
	auto setting = DoubleValue::Get(value.DefaultCastAs(LogicalType::DOUBLE));
	if (setting < 0) {
		return;
	}
	if (setting > 1) {
		throw BinderException("Setting %s must be between 0 and 1", name);
	}
	target = setting;
	has_target = true;
}

void ApplyNonNegativeDoubleSetting(ClientContext &context, const std::string &name, double &target, bool &has_target) {
	Value value;
	if (!TryGetSetting(context, name, value)) {
		return;
	}
	auto setting = DoubleValue::Get(value.DefaultCastAs(LogicalType::DOUBLE));
	if (setting == -1) {
		return;
	}
	if (!std::isfinite(setting) || setting < 0) {
		throw BinderException("Setting %s must be greater than or equal to 0, or -1 to disable", name);
	}
	target = setting;
	has_target = true;
}

void ApplyBigIntRangeSetting(ClientContext &context, const std::string &name, int64_t &target, bool &has_target,
                             int64_t min_value, int64_t max_value) {
	Value value;
	if (!TryGetSetting(context, name, value)) {
		return;
	}
	auto setting = BigIntValue::Get(value.DefaultCastAs(LogicalType::BIGINT));
	if (setting < 0) {
		return;
	}
	if (setting < min_value || setting > max_value) {
		throw BinderException("Setting %s must be between %lld and %lld", name, static_cast<long long>(min_value),
		                      static_cast<long long>(max_value));
	}
	target = setting;
	has_target = true;
}

void ApplyOptionalBigIntRangeSetting(ClientContext &context, const std::string &name, int64_t &target, bool &has_target,
                                     int64_t min_value, int64_t max_value) {
	Value value;
	if (!TryGetSetting(context, name, value)) {
		return;
	}
	auto setting = BigIntValue::Get(value.DefaultCastAs(LogicalType::BIGINT));
	if (setting == -1) {
		return;
	}
	if (setting < min_value || setting > max_value) {
		throw BinderException("Setting %s must be between %lld and %lld, or -1 to disable", name,
		                      static_cast<long long>(min_value), static_cast<long long>(max_value));
	}
	target = setting;
	has_target = true;
}

void ApplyTimeoutSetting(ClientContext &context, const std::string &name, duckdb_ai::CompletionOptions &options) {
	Value timeout_value;
	if (!TryGetSetting(context, name, timeout_value)) {
		return;
	}
	auto timeout_seconds = BigIntValue::Get(timeout_value.DefaultCastAs(LogicalType::BIGINT));
	if (timeout_seconds < 0) {
		throw BinderException("Setting %s must be greater than or equal to 0", name);
	}
	if (timeout_seconds > 0) {
		options.timeout_seconds = timeout_seconds;
		options.has_timeout_seconds = true;
	}
}

void ApplySettingsWithPrefix(ClientContext &context, const std::string &prefix, duckdb_ai::CompletionOptions &options) {
	ApplyStringSetting(context, prefix + "_provider", options.provider);
	ApplyStringSetting(context, prefix + "_model", options.model);
	ApplyStringSetting(context, prefix + "_base_url", options.base_url);
	ApplyStringSetting(context, prefix + "_response_format", options.response_format);
	ApplyStringSetting(context, prefix + "_response_schema", options.response_schema);
	ApplyStringSetting(context, prefix + "_log_endpoint", options.log_endpoint);
	ApplyStringSetting(context, prefix + "_log_format", options.log_format);
	ApplyStringSetting(context, prefix + "_log_tags", options.log_tags);
	ApplyBoolSetting(context, prefix + "_log_include_text", options.log_include_text, options.has_log_include_text);
	ApplyBoolSetting(context, prefix + "_log_strict", options.log_strict, options.has_log_strict);
	ApplyDoubleSetting(context, prefix + "_log_sample_rate", options.log_sample_rate, options.has_log_sample_rate);
	ApplyBigIntRangeSetting(context, prefix + "_retry_count", options.retry_count, options.has_retry_count, 0, 10);
	ApplyBigIntRangeSetting(context, prefix + "_retry_backoff_ms", options.retry_backoff_ms,
	                        options.has_retry_backoff_ms, 0, 60000);
	ApplyOptionalBigIntRangeSetting(context, prefix + "_max_concurrent_requests", options.max_concurrent_requests,
	                                options.has_max_concurrent_requests, 0, 1024);
	ApplyOptionalBigIntRangeSetting(context, prefix + "_min_request_interval_ms", options.min_request_interval_ms,
	                                options.has_min_request_interval_ms, 0, 60000);
	ApplyNonNegativeDoubleSetting(context, prefix + "_input_token_price_per_million",
	                              options.input_token_price_per_million, options.has_input_token_price_per_million);
	ApplyNonNegativeDoubleSetting(context, prefix + "_output_token_price_per_million",
	                              options.output_token_price_per_million, options.has_output_token_price_per_million);
	ApplyBoolSetting(context, prefix + "_use_builtin_model_prices", options.use_builtin_model_prices,
	                 options.has_use_builtin_model_prices);
	ApplyTimeoutSetting(context, prefix + "_timeout_seconds", options);
}

void ApplySettings(ClientContext &context, duckdb_ai::CompletionOptions &options) {
	ApplySettingsWithPrefix(context, "duckdb_ai", options);
	if (!options.response_format.empty()) {
		options.response_format = NormalizeResponseFormatValue(options.response_format, "AI setting");
	}
	if (!options.response_schema.empty()) {
		ValidateResponseSchemaValue(options.response_schema, "AI setting");
	}
	if (!options.log_format.empty()) {
		options.log_format = LowerAscii(options.log_format);
		if (options.log_format != "generic_json" && options.log_format != "generic" && options.log_format != "json" &&
		    options.log_format != "otlp_json" && options.log_format != "otlp") {
			throw BinderException("Setting duckdb_ai_log_format must be one of: generic_json, otlp_json");
		}
	}
}

bool TryReadSecretString(const KeyValueSecret &secret, const std::string &key, std::string &target) {
	Value value;
	if (!secret.TryGetValue(key, value) || value.IsNull()) {
		return false;
	}
	value = value.DefaultCastAs(LogicalType::VARCHAR);
	target = StringValue::Get(value);
	return !target.empty();
}

std::string OptionalSecretInputString(const CreateSecretInput &input, const std::string &name) {
	auto lookup = input.options.find(name);
	if (lookup == input.options.end() || lookup->second.IsNull()) {
		return "";
	}
	auto value = lookup->second;
	return StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
}

unique_ptr<BaseSecret> CreateAiProviderSecretFromConfig(ClientContext &, CreateSecretInput &input) {
	auto scope = input.scope;
	auto provider = OptionalSecretInputString(input, "ai_provider");
	if (provider.empty()) {
		throw InvalidInputException("duckdb_ai secrets require AI_PROVIDER");
	}
	provider = duckdb_ai::NormalizeProviderName(provider);
	if (scope.empty() && !provider.empty()) {
		scope.push_back(provider);
	}
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	secret->TrySetValue("api_key", input);
	secret->TrySetValue("model", input);
	secret->TrySetValue("base_url", input);
	secret->TrySetValue("api_version", input);
	if (!provider.empty()) {
		secret->secret_map["provider"] = Value(provider);
	}
	secret->redact_keys = {"api_key"};
	return std::move(secret);
}

void ApplyAiProviderSecret(ClientContext &context, duckdb_ai::CompletionOptions &options) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);

	unique_ptr<SecretEntry> secret_entry;
	bool explicit_secret = !options.secret_name.empty();
	if (explicit_secret) {
		secret_entry = secret_manager.GetSecretByName(transaction, options.secret_name);
		if (!secret_entry || !secret_entry->secret) {
			throw InvalidInputException("AI secret \"%s\" was not found", options.secret_name);
		}
		if (secret_entry->secret->GetType() != "duckdb_ai") {
			throw InvalidInputException("AI secret \"%s\" must have TYPE duckdb_ai", options.secret_name);
		}
	} else {
		auto lookup_path =
		    options.provider.empty() ? std::string() : duckdb_ai::NormalizeProviderName(options.provider);
		auto secret_match = secret_manager.LookupSecret(transaction, lookup_path, "duckdb_ai");
		if (!secret_match.HasMatch()) {
			return;
		}
		secret_entry = std::move(secret_match.secret_entry);
	}

	const auto &secret = dynamic_cast<const KeyValueSecret &>(*secret_entry->secret);
	std::string secret_provider;
	TryReadSecretString(secret, "provider", secret_provider);
	secret_provider = duckdb_ai::NormalizeProviderName(secret_provider);
	auto options_provider = duckdb_ai::NormalizeProviderName(options.provider);
	if (!secret_provider.empty() && !options.provider.empty() && secret_provider != options_provider) {
		if (explicit_secret) {
			throw InvalidInputException("AI secret \"%s\" is for provider \"%s\" but query uses provider \"%s\"",
			                            options.secret_name, secret_provider, options.provider);
		}
		return;
	}
	if (options.provider.empty() && !secret_provider.empty()) {
		options.provider = secret_provider;
	}
	if (options.model.empty()) {
		TryReadSecretString(secret, "model", options.model);
	}
	if (options.base_url.empty()) {
		TryReadSecretString(secret, "base_url", options.base_url);
	}
	if (options.api_key.empty()) {
		TryReadSecretString(secret, "api_key", options.api_key);
	}
}

unique_ptr<FunctionData> AiCompletionBindInternal(ClientContext &context, ScalarFunction &bound_function,
                                                  vector<unique_ptr<Expression>> &arguments,
                                                  bool validate_json_output = false) {
	auto bind_data = make_uniq<AiCompletionBindData>(false, validate_json_output);
	ApplySettings(context, bind_data->options);
	if (arguments.empty()) {
		throw BinderException("%s requires a prompt argument", bound_function.name);
	}

	idx_t positional_option_count = 0;
	for (idx_t i = 1; i < arguments.size(); i++) {
		auto alias = LowerAscii(arguments[i]->GetAlias());
		if (alias.empty()) {
			positional_option_count++;
			if (positional_option_count == 1) {
				bind_data->has_model_arg = true;
				bind_data->model_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else if (positional_option_count == 2) {
				bind_data->has_provider_arg = true;
				bind_data->provider_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else {
				throw BinderException("%s supports at most two positional options: model and provider",
				                      bound_function.name);
			}
			continue;
		}
		ApplyNamedOption(context, bind_data->options, *arguments[i], alias);
		bound_function.arguments.emplace_back(OptionType(alias));
	}
	bound_function.SetReturnType(LogicalType::VARCHAR);
	return std::move(bind_data);
}

unique_ptr<FunctionData> AiCompleteBind(ClientContext &context, ScalarFunction &bound_function,
                                        vector<unique_ptr<Expression>> &arguments) {
	return AiCompletionBindInternal(context, bound_function, arguments);
}

unique_ptr<FunctionData> AiRequestJsonBind(ClientContext &context, ScalarFunction &bound_function,
                                           vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = AiCompletionBindInternal(context, bound_function, arguments);
	bind_data->Cast<AiCompletionBindData>().request_json = true;
	return bind_data;
}

unique_ptr<FunctionData> AiCompleteJsonBind(ClientContext &context, ScalarFunction &bound_function,
                                            vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = AiCompletionBindInternal(context, bound_function, arguments, true);
	auto &completion_bind_data = bind_data->Cast<AiCompletionBindData>();
	auto response_format =
	    NormalizeResponseFormatValue(completion_bind_data.options.response_format, "ai_complete_json");
	if (!completion_bind_data.options.response_schema.empty()) {
		completion_bind_data.options.response_format = "json_schema";
	} else if (response_format.empty()) {
		completion_bind_data.options.response_format = "json_object";
	} else if (response_format == "text") {
		throw BinderException("ai_complete_json option \"response_format\" must be json_object or json_schema");
	}
	return bind_data;
}

struct StringVectorReader {
	// DuckDB vectors are chunk-oriented; cache the unified format once instead of per row.
	StringVectorReader(DataChunk &args, idx_t column) {
		args.data[column].ToUnifiedFormat(args.size(), data);
		values = UnifiedVectorFormat::GetData<string_t>(data);
	}

	bool Read(idx_t row, std::string &value) const {
		auto mapped_row = data.sel->get_index(row);
		if (!data.validity.RowIsValid(mapped_row)) {
			return false;
		}
		value = values[mapped_row].GetString();
		return true;
	}

	UnifiedVectorFormat data;
	const string_t *values;
};

unique_ptr<StringVectorReader> OptionalStringReader(DataChunk &args, bool enabled, idx_t column) {
	return enabled ? make_uniq<StringVectorReader>(args, column) : nullptr;
}

bool ReadRuntimeOptions(const duckdb_ai::CompletionOptions &base_options, bool has_model_arg,
                        const StringVectorReader *model_reader, bool has_provider_arg,
                        const StringVectorReader *provider_reader, idx_t row, duckdb_ai::CompletionOptions &options) {
	options = base_options;
	if (has_model_arg && !model_reader->Read(row, options.model)) {
		return false;
	}
	if (has_provider_arg && !provider_reader->Read(row, options.provider)) {
		return false;
	}
	return true;
}

std::string TrimJsonWhitespace(const std::string &input) {
	idx_t start = 0;
	while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
		start++;
	}
	idx_t end = input.size();
	while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
		end--;
	}
	return input.substr(start, end - start);
}

std::string BuildJsonCompletionPrompt(const std::string &prompt, const std::string &response_schema) {
	std::string output =
	    "Return only valid JSON. Do not include markdown, code fences, comments, or explanatory text.\n";
	if (!response_schema.empty()) {
		output += "The JSON response must follow this JSON Schema:\n";
		output += response_schema;
		output += "\n";
	} else {
		output += "The JSON response must be a top-level object or array.\n";
	}
	output += "\nRequest:\n";
	output += prompt;
	return output;
}

bool ValidateJsonCompletionOutput(const std::string &output, const std::string &response_schema, std::string &error) {
	auto trimmed = TrimJsonWhitespace(output);
	if (trimmed.empty()) {
		error = "empty response";
		return false;
	}
	if (trimmed[0] != '{' && trimmed[0] != '[') {
		error = "expected top-level JSON object or array";
		return false;
	}
	if (!duckdb_ai::ValidateJsonDocument(trimmed, error)) {
		return false;
	}
	if (!response_schema.empty() && !duckdb_ai::ValidateJsonAgainstSchema(trimmed, response_schema, error)) {
		error = "schema validation failed: " + error;
		return false;
	}
	return true;
}

void AiCompletionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiCompletionBindData>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<string_t>(result);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader prompt_reader(args, bind_data.prompt_index);
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	for (idx_t row = 0; row < args.size(); row++) {
		std::string prompt;
		if (!prompt_reader.Read(row, prompt)) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecret(state.GetContext(), options);
			auto request_prompt =
			    bind_data.validate_json_output ? BuildJsonCompletionPrompt(prompt, options.response_schema) : prompt;
			auto output = bind_data.request_json ? duckdb_ai::BuildRequestJson(request_prompt, options)
			                                     : duckdb_ai::Complete(request_prompt, options).text;
			if (bind_data.validate_json_output) {
				std::string error;
				if (!ValidateJsonCompletionOutput(output, options.response_schema, error)) {
					throw InvalidInputException("ai_complete_json expected valid JSON output: %s", error);
				}
			}
			result_data[row] = StringVector::AddString(result, output);
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}
}

std::string RequiredRecordStringInput(const TableFunctionBindInput &input, idx_t index, const std::string &name) {
	if (index >= input.inputs.size()) {
		throw BinderException("ai_complete_record requires a %s argument", name);
	}
	auto &value = input.inputs[index];
	if (value.IsNull()) {
		throw BinderException("ai_complete_record %s argument must not be NULL", name);
	}
	return StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
}

std::string OptionalRecordStringInput(const TableFunctionBindInput &input, idx_t index) {
	if (index >= input.inputs.size()) {
		return "";
	}
	auto &value = input.inputs[index];
	if (value.IsNull()) {
		throw BinderException("ai_complete_record positional options must not be NULL");
	}
	return StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
}

AiRecordColumn BuildAiRecordColumn(const duckdb_ai::JsonSchemaProperty &property);

LogicalType AiRecordStructType(const vector<AiRecordColumn> &columns) {
	child_list_t<LogicalType> children;
	for (auto &column : columns) {
		children.emplace_back(column.name, column.type);
	}
	return LogicalType::STRUCT(std::move(children));
}

LogicalType AiRecordPropertyType(const duckdb_ai::JsonSchemaProperty &property) {
	auto type = LowerAscii(property.type);
	if (type == "string") {
		return LogicalType::VARCHAR;
	}
	if (type == "boolean") {
		return LogicalType::BOOLEAN;
	}
	if (type == "integer") {
		return LogicalType::BIGINT;
	}
	if (type == "number") {
		return LogicalType::DOUBLE;
	}
	if (type == "object" && !property.children.empty()) {
		vector<AiRecordColumn> children;
		for (auto &child : property.children) {
			children.push_back(BuildAiRecordColumn(child));
		}
		return AiRecordStructType(children);
	}
	if (type == "array") {
		auto item_type = LowerAscii(property.item_type);
		if (item_type == "string") {
			return LogicalType::LIST(LogicalType::VARCHAR);
		}
		if (item_type == "boolean") {
			return LogicalType::LIST(LogicalType::BOOLEAN);
		}
		if (item_type == "integer") {
			return LogicalType::LIST(LogicalType::BIGINT);
		}
		if (item_type == "number") {
			return LogicalType::LIST(LogicalType::DOUBLE);
		}
		if (item_type == "object" && !property.item_children.empty()) {
			vector<AiRecordColumn> children;
			for (auto &child : property.item_children) {
				children.push_back(BuildAiRecordColumn(child));
			}
			return LogicalType::LIST(AiRecordStructType(children));
		}
	}
	return LogicalType::VARCHAR;
}

AiRecordColumn BuildAiRecordColumn(const duckdb_ai::JsonSchemaProperty &property) {
	AiRecordColumn column;
	column.name = property.name;
	column.type = AiRecordPropertyType(property);
	column.schema_type = property.type;
	column.item_schema_type = property.item_type;
	for (auto &child : property.children) {
		column.children.push_back(BuildAiRecordColumn(child));
	}
	for (auto &child : property.item_children) {
		column.item_children.push_back(BuildAiRecordColumn(child));
	}
	return column;
}

void ApplyAiRecordValueOption(duckdb_ai::CompletionOptions &options, const std::string &name, const Value &value_p) {
	if (!ApplyCompletionValueOption(options, "ai_complete_record", name, value_p, false, true)) {
		throw BinderException("Unsupported ai_complete_record option \"%s\"", name);
	}
}

const duckdb_ai::JsonExtractedValue *FindExtractedField(const duckdb_ai::JsonExtractedValue &value,
                                                        const std::string &name) {
	for (auto &field : value.object_values) {
		if (field.first == name) {
			return &field.second;
		}
	}
	return nullptr;
}

Value AiRecordValue(const duckdb_ai::JsonExtractedValue &value, const AiRecordColumn &column) {
	if (value.kind == duckdb_ai::JsonExtractedKind::MISSING || value.kind == duckdb_ai::JsonExtractedKind::NULL_VALUE) {
		return Value(column.type);
	}
	switch (column.type.id()) {
	case LogicalTypeId::VARCHAR:
		if (value.kind == duckdb_ai::JsonExtractedKind::STRING) {
			return Value(value.string_value);
		}
		return Value(value.json_value);
	case LogicalTypeId::BOOLEAN:
		if (value.kind != duckdb_ai::JsonExtractedKind::BOOLEAN) {
			throw InvalidInputException("ai_complete_record field \"%s\" expected boolean", column.name);
		}
		return Value::BOOLEAN(value.boolean_value);
	case LogicalTypeId::BIGINT:
		if (value.kind != duckdb_ai::JsonExtractedKind::NUMBER || !value.number_is_integer) {
			throw InvalidInputException("ai_complete_record field \"%s\" expected integer", column.name);
		}
		if (value.number_value < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
		    value.number_value > static_cast<double>(std::numeric_limits<int64_t>::max())) {
			throw InvalidInputException("ai_complete_record field \"%s\" integer is out of BIGINT range", column.name);
		}
		return Value::BIGINT(static_cast<int64_t>(value.number_value));
	case LogicalTypeId::DOUBLE:
		if (value.kind != duckdb_ai::JsonExtractedKind::NUMBER) {
			throw InvalidInputException("ai_complete_record field \"%s\" expected number", column.name);
		}
		return Value::DOUBLE(value.number_value);
	case LogicalTypeId::STRUCT: {
		if (value.kind != duckdb_ai::JsonExtractedKind::OBJECT) {
			throw InvalidInputException("ai_complete_record field \"%s\" expected object", column.name);
		}
		vector<Value> children;
		children.reserve(column.children.size());
		for (auto &child_column : column.children) {
			auto child_value = FindExtractedField(value, child_column.name);
			children.push_back(child_value ? AiRecordValue(*child_value, child_column) : Value(child_column.type));
		}
		return Value::STRUCT(column.type, std::move(children));
	}
	case LogicalTypeId::LIST: {
		if (value.kind != duckdb_ai::JsonExtractedKind::ARRAY) {
			throw InvalidInputException("ai_complete_record field \"%s\" expected array", column.name);
		}
		auto child_type = ListType::GetChildType(column.type);
		AiRecordColumn child_column;
		child_column.name = column.name + "[]";
		child_column.type = child_type;
		child_column.schema_type = column.item_schema_type;
		child_column.children = column.item_children;
		vector<Value> children;
		children.reserve(value.array_values.size());
		for (auto &item : value.array_values) {
			children.push_back(AiRecordValue(item, child_column));
		}
		return Value::LIST(child_type, std::move(children));
	}
	default:
		break;
	}
	return Value(value.json_value);
}

unique_ptr<FunctionData> AiRecordBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() < 2 || input.inputs.size() > 4) {
		throw BinderException("ai_complete_record expects prompt, response_schema, optional model, optional provider");
	}
	auto bind_data = make_uniq<AiRecordBindData>();
	ApplySettings(context, bind_data->options);
	bind_data->prompt = RequiredRecordStringInput(input, 0, "prompt");
	bind_data->response_schema = RequiredRecordStringInput(input, 1, "response_schema");
	ValidateResponseSchemaValue(bind_data->response_schema, "ai_complete_record");
	bind_data->options.response_schema = bind_data->response_schema;
	bind_data->options.response_format = "json_schema";
	if (input.inputs.size() > 2) {
		bind_data->options.model = OptionalRecordStringInput(input, 2);
	}
	if (input.inputs.size() > 3) {
		bind_data->options.provider = OptionalRecordStringInput(input, 3);
	}
	for (auto &named_parameter : input.named_parameters) {
		ApplyAiRecordValueOption(bind_data->options, LowerAscii(named_parameter.first), named_parameter.second);
	}

	vector<duckdb_ai::JsonSchemaProperty> properties;
	std::string error;
	if (!duckdb_ai::ExtractJsonSchemaProperties(bind_data->response_schema, properties, error)) {
		throw BinderException("ai_complete_record response_schema cannot be projected as a record: %s", error);
	}
	for (auto &property : properties) {
		auto column = BuildAiRecordColumn(property);
		names.emplace_back(column.name);
		return_types.emplace_back(column.type);
		bind_data->columns.push_back(std::move(column));
	}
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> AiRecordInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<AiRecordScanData>();
}

void AiRecordFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<AiRecordScanData>();
	if (state.emitted) {
		return;
	}
	auto &bind_data = input.bind_data->Cast<AiRecordBindData>();
	try {
		auto options = bind_data.options;
		ApplyAiProviderSecret(context, options);
		auto request_prompt = BuildJsonCompletionPrompt(bind_data.prompt, bind_data.response_schema);
		auto result = duckdb_ai::Complete(request_prompt, options).text;
		std::string error;
		if (!ValidateJsonCompletionOutput(result, bind_data.response_schema, error)) {
			throw InvalidInputException("ai_complete_record expected valid JSON output: %s", error);
		}
		vector<std::string> field_names;
		field_names.reserve(bind_data.columns.size());
		for (auto &column : bind_data.columns) {
			field_names.push_back(column.name);
		}
		vector<duckdb_ai::JsonExtractedValue> values;
		if (!duckdb_ai::ExtractJsonObjectFields(result, field_names, values, error)) {
			throw InvalidInputException("ai_complete_record could not project JSON output: %s", error);
		}
		for (idx_t column_index = 0; column_index < bind_data.columns.size(); column_index++) {
			output.SetValue(column_index, 0, AiRecordValue(values[column_index], bind_data.columns[column_index]));
		}
		output.SetCardinality(1);
	} catch (std::exception &) {
		if (bind_data.options.fail_on_error) {
			throw;
		}
		for (idx_t column_index = 0; column_index < bind_data.columns.size(); column_index++) {
			output.SetValue(column_index, 0, Value(bind_data.columns[column_index].type));
		}
		output.SetCardinality(1);
	}
	state.emitted = true;
}

bool IsEmbeddingOption(const std::string &name) {
	return name == "model" || name == "provider" || name == "profile" || name == "secret" || name == "secret_name" ||
	       name == "base_url" || name == "timeout_seconds" || name == "retry_count" || name == "retry_backoff_ms" ||
	       name == "max_concurrent_requests" || name == "min_request_interval_ms" ||
	       name == "input_token_price_per_million" || name == "output_token_price_per_million" ||
	       name == "use_builtin_model_prices" || name == "fail_on_error" || name == "log_format" ||
	       name == "log_tags" || name == "log_sample_rate";
}

unique_ptr<FunctionData> AiEmbeddingBindInternal(ClientContext &context, ScalarFunction &bound_function,
                                                 vector<unique_ptr<Expression>> &arguments, bool request_json) {
	auto bind_data = make_uniq<AiCompletionBindData>(request_json);
	ApplySettings(context, bind_data->options);
	if (arguments.empty()) {
		throw BinderException("%s requires an input argument", bound_function.name);
	}

	idx_t positional_option_count = 0;
	for (idx_t i = 1; i < arguments.size(); i++) {
		auto alias = LowerAscii(arguments[i]->GetAlias());
		if (alias.empty()) {
			positional_option_count++;
			if (positional_option_count == 1) {
				bind_data->has_model_arg = true;
				bind_data->model_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else if (positional_option_count == 2) {
				bind_data->has_provider_arg = true;
				bind_data->provider_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else {
				throw BinderException("%s supports at most two positional options: model and provider",
				                      bound_function.name);
			}
			continue;
		}
		if (!IsEmbeddingOption(alias)) {
			throw BinderException(
			    "Unsupported AI embedding option \"%s\". Supported options: model, provider, "
			    "profile, secret, base_url, timeout_seconds, retry_count, retry_backoff_ms, fail_on_error, "
			    "max_concurrent_requests, min_request_interval_ms, "
			    "input_token_price_per_million, output_token_price_per_million, "
			    "use_builtin_model_prices, log_format, log_tags, log_sample_rate",
			    alias);
		}
		ApplyNamedOption(context, bind_data->options, *arguments[i], alias);
		bound_function.arguments.emplace_back(OptionType(alias));
	}
	bound_function.SetReturnType(request_json ? LogicalType::VARCHAR : LogicalType::LIST(LogicalType::DOUBLE));
	return std::move(bind_data);
}

unique_ptr<FunctionData> AiEmbedBind(ClientContext &context, ScalarFunction &bound_function,
                                     vector<unique_ptr<Expression>> &arguments) {
	return AiEmbeddingBindInternal(context, bound_function, arguments, false);
}

unique_ptr<FunctionData> AiEmbeddingRequestJsonBind(ClientContext &context, ScalarFunction &bound_function,
                                                    vector<unique_ptr<Expression>> &arguments) {
	return AiEmbeddingBindInternal(context, bound_function, arguments, true);
}

unique_ptr<FunctionData> AiSimilarityBind(ClientContext &context, ScalarFunction &bound_function,
                                          vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<AiCompletionBindData>(false);
	ApplySettings(context, bind_data->options);
	if (arguments.size() < 2) {
		throw BinderException("%s requires two text arguments", bound_function.name);
	}

	idx_t positional_option_count = 0;
	for (idx_t i = 2; i < arguments.size(); i++) {
		auto alias = LowerAscii(arguments[i]->GetAlias());
		if (alias.empty()) {
			positional_option_count++;
			if (positional_option_count == 1) {
				bind_data->has_model_arg = true;
				bind_data->model_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else if (positional_option_count == 2) {
				bind_data->has_provider_arg = true;
				bind_data->provider_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else {
				throw BinderException("%s supports at most two positional options: model and provider",
				                      bound_function.name);
			}
			continue;
		}
		if (!IsEmbeddingOption(alias)) {
			throw BinderException(
			    "Unsupported AI similarity option \"%s\". Supported options: model, provider, "
			    "profile, secret, base_url, timeout_seconds, retry_count, retry_backoff_ms, fail_on_error, "
			    "max_concurrent_requests, min_request_interval_ms, "
			    "input_token_price_per_million, output_token_price_per_million, "
			    "use_builtin_model_prices, log_format, log_tags, log_sample_rate",
			    alias);
		}
		ApplyNamedOption(context, bind_data->options, *arguments[i], alias);
		bound_function.arguments.emplace_back(OptionType(alias));
	}
	bound_function.SetReturnType(LogicalType::DOUBLE);
	return std::move(bind_data);
}

void ReadEmbeddingInputs(const StringVectorReader &input_reader, const StringVectorReader *model_reader,
                         const StringVectorReader *provider_reader, const AiCompletionBindData &bind_data, idx_t row,
                         std::string &input, duckdb_ai::CompletionOptions &options, bool &is_null) {
	if (!input_reader.Read(row, input)) {
		is_null = true;
		return;
	}
	if (!ReadRuntimeOptions(bind_data.options, bind_data.has_model_arg, model_reader, bind_data.has_provider_arg,
	                        provider_reader, row, options)) {
		is_null = true;
	}
}

double CosineSimilarity(const std::vector<double> &left, const std::vector<double> &right) {
	if (left.empty() || right.empty() || left.size() != right.size()) {
		throw InvalidInputException("AI embeddings must have the same non-zero dimension for ai_similarity");
	}

	double dot = 0;
	double left_norm = 0;
	double right_norm = 0;
	for (idx_t i = 0; i < left.size(); i++) {
		dot += left[i] * right[i];
		left_norm += left[i] * left[i];
		right_norm += right[i] * right[i];
	}
	if (left_norm == 0 || right_norm == 0) {
		throw InvalidInputException("AI embeddings must have non-zero norm for ai_similarity");
	}
	return dot / (std::sqrt(left_norm) * std::sqrt(right_norm));
}

void AiSimilarityFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiCompletionBindData>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader left_reader(args, 0);
	StringVectorReader right_reader(args, 1);
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	for (idx_t row = 0; row < args.size(); row++) {
		std::string left;
		std::string right;
		if (!left_reader.Read(row, left) || !right_reader.Read(row, right)) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecret(state.GetContext(), options);
			auto left_embedding = duckdb_ai::Embed(left, options);
			auto right_embedding = duckdb_ai::Embed(right, options);
			result_data[row] = CosineSimilarity(left_embedding.values, right_embedding.values);
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}
}

void AiEmbeddingRequestJsonFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiCompletionBindData>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<string_t>(result);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader input_reader(args, bind_data.prompt_index);
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		duckdb_ai::CompletionOptions options;
		bool is_null = false;
		ReadEmbeddingInputs(input_reader, model_reader.get(), provider_reader.get(), bind_data, row, input, options,
		                    is_null);
		if (is_null) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecret(state.GetContext(), options);
			auto output = duckdb_ai::BuildEmbeddingRequestJson(input, options);
			result_data[row] = StringVector::AddString(result, output);
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}
}

void AiEmbedFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiCompletionBindData>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<list_entry_t>(result);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader input_reader(args, bind_data.prompt_index);
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		duckdb_ai::CompletionOptions options;
		bool is_null = false;
		ReadEmbeddingInputs(input_reader, model_reader.get(), provider_reader.get(), bind_data, row, input, options,
		                    is_null);
		if (is_null) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecret(state.GetContext(), options);
			auto embedding = duckdb_ai::Embed(input, options);
			result_data[row].offset = ListVector::GetListSize(result);
			result_data[row].length = embedding.values.size();
			for (auto value : embedding.values) {
				ListVector::PushBack(result, Value::DOUBLE(value));
			}
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}
}

unique_ptr<FunctionData> AiTaskBindInternal(ClientContext &context, ScalarFunction &bound_function,
                                            vector<unique_ptr<Expression>> &arguments, AiTaskKind task,
                                            idx_t required_args) {
	auto bind_data = make_uniq<AiTaskBindData>(task, required_args);
	ApplySettings(context, bind_data->options);
	if (arguments.size() < required_args) {
		throw BinderException("%s requires %llu argument(s)", bound_function.name, required_args);
	}

	idx_t positional_option_count = 0;
	for (idx_t i = required_args; i < arguments.size(); i++) {
		auto alias = LowerAscii(arguments[i]->GetAlias());
		if (alias.empty()) {
			positional_option_count++;
			if (positional_option_count == 1) {
				bind_data->has_model_arg = true;
				bind_data->model_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else if (positional_option_count == 2) {
				bind_data->has_provider_arg = true;
				bind_data->provider_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else {
				throw BinderException("%s supports at most two positional options: model and provider",
				                      bound_function.name);
			}
			continue;
		}
		ApplyNamedOption(context, bind_data->options, *arguments[i], alias);
		bound_function.arguments.emplace_back(OptionType(alias));
	}
	bound_function.SetReturnType(LogicalType::VARCHAR);
	return std::move(bind_data);
}

unique_ptr<FunctionData> AiSummarizeBind(ClientContext &context, ScalarFunction &bound_function,
                                         vector<unique_ptr<Expression>> &arguments) {
	return AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::SUMMARIZE, 1);
}

unique_ptr<FunctionData> AiSentimentBind(ClientContext &context, ScalarFunction &bound_function,
                                         vector<unique_ptr<Expression>> &arguments) {
	return AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::SENTIMENT, 1);
}

unique_ptr<FunctionData> AiFixGrammarBind(ClientContext &context, ScalarFunction &bound_function,
                                          vector<unique_ptr<Expression>> &arguments) {
	return AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::FIX_GRAMMAR, 1);
}

unique_ptr<FunctionData> AiMaskBind(ClientContext &context, ScalarFunction &bound_function,
                                    vector<unique_ptr<Expression>> &arguments) {
	return AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::MASK, 1);
}

unique_ptr<FunctionData> AiTranslateBind(ClientContext &context, ScalarFunction &bound_function,
                                         vector<unique_ptr<Expression>> &arguments) {
	return AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::TRANSLATE, 2);
}

unique_ptr<FunctionData> AiClassifyBind(ClientContext &context, ScalarFunction &bound_function,
                                        vector<unique_ptr<Expression>> &arguments) {
	return AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::CLASSIFY, 2);
}

unique_ptr<FunctionData> AiExtractBind(ClientContext &context, ScalarFunction &bound_function,
                                       vector<unique_ptr<Expression>> &arguments) {
	return AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::EXTRACT, 2);
}

unique_ptr<FunctionData> AiFilterBind(ClientContext &context, ScalarFunction &bound_function,
                                      vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::FILTER, 2);
	bound_function.SetReturnType(LogicalType::BOOLEAN);
	return bind_data;
}

unique_ptr<FunctionData> AiPromptSqlBind(ClientContext &context, ScalarFunction &bound_function,
                                         vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<AiPromptSqlBindData>();
	ApplySettings(context, bind_data->options);
	if (arguments.empty()) {
		throw BinderException("%s requires a question argument", bound_function.name);
	}

	idx_t positional_option_count = 0;
	for (idx_t i = 1; i < arguments.size(); i++) {
		auto alias = LowerAscii(arguments[i]->GetAlias());
		if (alias.empty()) {
			positional_option_count++;
			if (positional_option_count == 1) {
				bind_data->has_schema_context_arg = true;
				bind_data->schema_context_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else if (positional_option_count == 2) {
				bind_data->has_model_arg = true;
				bind_data->model_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else if (positional_option_count == 3) {
				bind_data->has_provider_arg = true;
				bind_data->provider_index = i;
				bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			} else {
				throw BinderException("%s supports at most three positional options: schema_context, model, and "
				                      "provider",
				                      bound_function.name);
			}
			continue;
		}
		if (alias == "schema_context" || alias == "schema") {
			auto value = EvaluateConstantOption(context, *arguments[i], alias, LogicalType::VARCHAR);
			bind_data->schema_context = StringValue::Get(value);
			bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			continue;
		}
		if (alias == "include_tables") {
			auto value = EvaluateConstantOption(context, *arguments[i], alias, LogicalType::LIST(LogicalType::VARCHAR));
			bind_data->schema_options.include_tables =
			    ReadIncludeTablesValue(value, "include_tables", bound_function.name);
			bound_function.arguments.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
			continue;
		}
		if (alias == "exclude_tables") {
			auto value = EvaluateConstantOption(context, *arguments[i], alias, LogicalType::LIST(LogicalType::VARCHAR));
			bind_data->schema_options.exclude_tables =
			    ReadIncludeTablesValue(value, "exclude_tables", bound_function.name);
			bound_function.arguments.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
			continue;
		}
		if (alias == "sample_rows") {
			auto value = EvaluateConstantOption(context, *arguments[i], alias, LogicalType::BIGINT);
			bind_data->schema_options.sample_rows = ReadSampleRowsValue(value, bound_function.name);
			bound_function.arguments.emplace_back(LogicalType::BIGINT);
			continue;
		}
		ApplyNamedOption(context, bind_data->options, *arguments[i], alias);
		bound_function.arguments.emplace_back(PromptSqlOptionType(alias));
	}
	bound_function.SetReturnType(LogicalType::VARCHAR);
	return std::move(bind_data);
}

std::string EvaluateConstantString(ClientContext &context, Expression &expr, const std::string &name) {
	auto value = EvaluateConstantOption(context, expr, name, LogicalType::VARCHAR);
	return StringValue::Get(value);
}

unique_ptr<FunctionData> AiAggregateBindInternal(ClientContext &context, AggregateFunction &function,
                                                 vector<unique_ptr<Expression>> &arguments, AiAggregateKind kind) {
	auto bind_data = make_uniq<AiAggregateBindData>(kind);
	ApplySettings(context, bind_data->options);
	if (arguments.empty()) {
		throw BinderException("%s requires an input text argument", function.name);
	}

	bool has_instruction = kind == AiAggregateKind::SUMMARIZE;
	idx_t positional_option_count = 0;
	vector<idx_t> erase_indexes;
	for (idx_t i = 1; i < arguments.size(); i++) {
		auto alias = LowerAscii(arguments[i]->GetAlias());
		if (alias.empty()) {
			if (!has_instruction) {
				bind_data->instruction = EvaluateConstantString(context, *arguments[i], "instruction");
				has_instruction = true;
				erase_indexes.push_back(i);
				continue;
			}
			positional_option_count++;
			if (positional_option_count == 1) {
				bind_data->options.model = EvaluateConstantString(context, *arguments[i], "model");
			} else if (positional_option_count == 2) {
				bind_data->options.provider = EvaluateConstantString(context, *arguments[i], "provider");
			} else {
				throw BinderException("%s supports at most two positional options after the instruction: model and "
				                      "provider",
				                      function.name);
			}
			erase_indexes.push_back(i);
			continue;
		}
		ApplyAggregateOption(context, *bind_data, *arguments[i], alias);
		erase_indexes.push_back(i);
	}
	if (!has_instruction) {
		throw BinderException("%s requires a constant instruction argument", function.name);
	}
	ApplyAiProviderSecret(context, bind_data->options);

	for (idx_t erase_offset = erase_indexes.size(); erase_offset > 0; erase_offset--) {
		Function::EraseArgument(function, arguments, erase_indexes[erase_offset - 1]);
	}
	function.SetReturnType(LogicalType::VARCHAR);
	return std::move(bind_data);
}

unique_ptr<FunctionData> AiAggBind(ClientContext &context, AggregateFunction &function,
                                   vector<unique_ptr<Expression>> &arguments) {
	return AiAggregateBindInternal(context, function, arguments, AiAggregateKind::GENERIC);
}

unique_ptr<FunctionData> AiSummarizeAggBind(ClientContext &context, AggregateFunction &function,
                                            vector<unique_ptr<Expression>> &arguments) {
	return AiAggregateBindInternal(context, function, arguments, AiAggregateKind::SUMMARIZE);
}

void AiAggregateEnsureCapacity(AiAggregateState &state, ArenaAllocator &allocator, idx_t required_size) {
	if (state.alloc_size >= required_size) {
		return;
	}
	auto new_size = state.alloc_size == 0 ? MaxValue<idx_t>(8, required_size) : state.alloc_size;
	while (new_size < required_size) {
		new_size *= 2;
	}
	if (!state.dataptr) {
		state.dataptr = char_ptr_cast(allocator.Allocate(new_size));
	} else {
		state.dataptr = char_ptr_cast(allocator.Reallocate(data_ptr_cast(state.dataptr), state.alloc_size, new_size));
	}
	state.alloc_size = new_size;
}

void AiAggregateAppendBytes(AiAggregateState &state, ArenaAllocator &allocator, const char *data, idx_t size) {
	if (size == 0) {
		return;
	}
	AiAggregateEnsureCapacity(state, allocator, state.size + size);
	memcpy(state.dataptr + state.size, data, size);
	state.size += size;
}

void AiAggregateAppend(AiAggregateState &state, ArenaAllocator &allocator, const char *data, idx_t size,
                       const AiAggregateBindData &bind_data) {
	state.row_count++;
	if (state.size >= bind_data.max_context_chars) {
		state.truncated = true;
		return;
	}

	auto separator_size = state.size == 0 ? 0 : bind_data.separator.size();
	auto available = bind_data.max_context_chars - state.size;
	if (separator_size >= available) {
		state.truncated = true;
		return;
	}
	if (separator_size > 0) {
		AiAggregateAppendBytes(state, allocator, bind_data.separator.c_str(), separator_size);
		available -= separator_size;
	}

	auto append_size = MinValue<idx_t>(size, available);
	AiAggregateAppendBytes(state, allocator, data, append_size);
	if (append_size < size) {
		state.truncated = true;
	}
}

std::string BuildAiAggregatePrompt(const AiAggregateBindData &bind_data, const AiAggregateState &state) {
	std::string values;
	if (state.dataptr && state.size > 0) {
		values.assign(state.dataptr, state.size);
	}
	std::string prompt;
	if (bind_data.kind == AiAggregateKind::SUMMARIZE) {
		prompt =
		    "Summarize the following SQL aggregate input values concisely. Return only the summary.\n\nInput values:\n";
	} else {
		prompt =
		    "Use the following SQL aggregate input values to answer the instruction. Return only the final answer.\n\n"
		    "Instruction:\n" +
		    bind_data.instruction + "\n\nInput values:\n";
	}
	prompt += values;
	if (state.truncated) {
		prompt += "\n\nNote: the input values were truncated to fit the configured context limit.";
	}
	prompt += "\n\nInput row count: " + std::to_string(state.row_count);
	return prompt;
}

struct AiAggregateOperation {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.size = 0;
		state.alloc_size = 0;
		state.row_count = 0;
		state.truncated = false;
		state.dataptr = nullptr;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		auto &bind_data = unary_input.input.bind_data->Cast<AiAggregateBindData>();
		AiAggregateAppend(state, unary_input.input.allocator, input.GetData(), input.GetSize(), bind_data);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		if (!source.dataptr) {
			target.row_count += source.row_count;
			target.truncated = target.truncated || source.truncated;
			return;
		}
		auto &bind_data = aggr_input_data.bind_data->Cast<AiAggregateBindData>();
		if (target.row_count == 0) {
			target.row_count = source.row_count;
			AiAggregateAppend(target, aggr_input_data.allocator, source.dataptr, source.size, bind_data);
			target.row_count = source.row_count;
		} else {
			auto before_count = target.row_count;
			AiAggregateAppend(target, aggr_input_data.allocator, source.dataptr, source.size, bind_data);
			target.row_count = before_count + source.row_count;
		}
		target.truncated = target.truncated || source.truncated;
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.row_count == 0) {
			finalize_data.ReturnNull();
			return;
		}
		auto &bind_data = finalize_data.input.bind_data->Cast<AiAggregateBindData>();
		try {
			auto prompt = BuildAiAggregatePrompt(bind_data, state);
			auto output = duckdb_ai::Complete(prompt, bind_data.options).text;
			target = finalize_data.ReturnString(string_t(output));
		} catch (std::exception &ex) {
			if (bind_data.options.fail_on_error) {
				throw;
			}
			finalize_data.ReturnNull();
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

std::string BuildTaskPrompt(AiTaskKind task, const std::string &input, const std::string &parameter) {
	switch (task) {
	case AiTaskKind::SUMMARIZE:
		return "Summarize the following text concisely. Return only the summary.\n\nText:\n" + input;
	case AiTaskKind::SENTIMENT:
		return "Classify the sentiment of the following text as positive, neutral, or negative. Return only one "
		       "label.\n\nText:\n" +
		       input;
	case AiTaskKind::FIX_GRAMMAR:
		return "Fix grammar, spelling, and punctuation in the following text. Preserve the original meaning and "
		       "return only the corrected text.\n\nText:\n" +
		       input;
	case AiTaskKind::MASK:
		return "Mask direct personal data, credentials, secrets, and payment identifiers in the following text. "
		       "Return only the redacted text.\n\nText:\n" +
		       input;
	case AiTaskKind::TRANSLATE:
		return "Translate the following text to " + parameter +
		       ". Preserve meaning and formatting. Return only the translation.\n\nText:\n" + input;
	case AiTaskKind::CLASSIFY:
		return "Classify the following text into exactly one of these labels: " + parameter +
		       ". Return only the chosen label.\n\nText:\n" + input;
	case AiTaskKind::EXTRACT:
		return "Extract the requested information from the following text. Return concise JSON when the request "
		       "asks for structured data.\n\nExtraction request:\n" +
		       parameter + "\n\nText:\n" + input;
	case AiTaskKind::FILTER:
		return "Evaluate whether the following text matches the natural-language predicate. Return only true or "
		       "false.\n\nPredicate:\n" +
		       parameter + "\n\nText:\n" + input;
	default:
		throw InternalException("Unknown AI task kind");
	}
}

bool IsAsciiWhitespace(char c) {
	return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
}

std::string TrimAscii(const std::string &input) {
	idx_t start = 0;
	while (start < input.size() && IsAsciiWhitespace(input[start])) {
		start++;
	}
	idx_t end = input.size();
	while (end > start && IsAsciiWhitespace(input[end - 1])) {
		end--;
	}
	return input.substr(start, end - start);
}

bool StartsWith(const std::string &input, const std::string &prefix) {
	return input.size() >= prefix.size() && input.compare(0, prefix.size(), prefix) == 0;
}

std::string StripMarkdownSqlFence(const std::string &input) {
	auto output = TrimAscii(input);
	if (output.size() >= 2 && output.front() == '`' && output.back() == '`' && output.find('\n') == std::string::npos) {
		return TrimAscii(output.substr(1, output.size() - 2));
	}
	if (!StartsWith(output, "```")) {
		return output;
	}

	auto first_newline = output.find('\n');
	if (first_newline == std::string::npos) {
		return output;
	}
	auto close_fence = output.rfind("```");
	if (close_fence == 0 || close_fence == std::string::npos) {
		return TrimAscii(output.substr(first_newline + 1));
	}
	return TrimAscii(output.substr(first_newline + 1, close_fence - first_newline - 1));
}

std::string BuildPromptSqlPrompt(const std::string &question, const std::string &schema_context) {
	std::string prompt =
	    "Generate one DuckDB SQL SELECT statement for the request below.\n"
	    "Rules:\n"
	    "- Return only SQL text.\n"
	    "- Do not wrap the answer in markdown or code fences.\n"
	    "- The SQL must be read-only and must contain exactly one SELECT statement.\n"
	    "- Do not include INSERT, UPDATE, DELETE, CREATE, DROP, ALTER, COPY, ATTACH, INSTALL, LOAD, CALL, PRAGMA, or "
	    "multiple statements.\n";
	if (!schema_context.empty()) {
		prompt += "\nAvailable schema/context:\n";
		prompt += schema_context;
		prompt += "\n";
	}
	prompt += "\nRequest:\n";
	prompt += question;
	return prompt;
}

std::string QuoteSqlIdentifier(const std::string &identifier) {
	std::string output = "\"";
	for (auto c : identifier) {
		if (c == '"') {
			output += "\"\"";
		} else {
			output.push_back(c);
		}
	}
	output.push_back('"');
	return output;
}

std::string QualifiedTableName(const SchemaTableInfo &table) {
	return QuoteSqlIdentifier(table.catalog) + "." + QuoteSqlIdentifier(table.schema) + "." +
	       QuoteSqlIdentifier(table.table);
}

std::string ValueToOptionalString(const Value &value_p) {
	if (value_p.IsNull()) {
		return "";
	}
	auto value = value_p;
	return StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
}

vector<std::string> SplitQualifiedIdentifier(const std::string &identifier) {
	vector<std::string> parts;
	std::string current;
	bool in_quotes = false;
	for (idx_t i = 0; i < identifier.size(); i++) {
		auto c = identifier[i];
		if (c == '"') {
			if (in_quotes && i + 1 < identifier.size() && identifier[i + 1] == '"') {
				current.push_back('"');
				i++;
			} else {
				in_quotes = !in_quotes;
			}
			continue;
		}
		if (c == '.' && !in_quotes) {
			parts.push_back(TrimAscii(current));
			current.clear();
			continue;
		}
		current.push_back(c);
	}
	parts.push_back(TrimAscii(current));
	return parts;
}

struct TableSpec {
	vector<std::string> parts;
};

vector<TableSpec> ParseTableSpecs(const vector<std::string> &tables, const std::string &option_name) {
	vector<TableSpec> specs;
	for (auto &table : tables) {
		auto trimmed = TrimAscii(table);
		if (trimmed.empty()) {
			throw BinderException("ai_schema_prompt %s entries must not be empty", option_name);
		}
		auto parts = SplitQualifiedIdentifier(trimmed);
		if (parts.empty() || parts.size() > 3) {
			throw BinderException("ai_schema_prompt %s entries must be table, schema.table, or "
			                      "catalog.schema.table names",
			                      option_name);
		}
		for (auto &part : parts) {
			if (part.empty()) {
				throw BinderException("ai_schema_prompt %s entries must not contain empty identifier parts",
				                      option_name);
			}
			part = LowerAscii(part);
		}
		specs.push_back({std::move(parts)});
	}
	return specs;
}

bool MatchesTableSpec(const SchemaTableInfo &table, const TableSpec &spec) {
	auto catalog = LowerAscii(table.catalog);
	auto schema = LowerAscii(table.schema);
	auto table_name = LowerAscii(table.table);
	if (spec.parts.size() == 1) {
		return table_name == spec.parts[0];
	}
	if (spec.parts.size() == 2) {
		return schema == spec.parts[0] && table_name == spec.parts[1];
	}
	return catalog == spec.parts[0] && schema == spec.parts[1] && table_name == spec.parts[2];
}

bool MatchesAnyTableSpec(const SchemaTableInfo &table, const vector<TableSpec> &specs) {
	for (auto &spec : specs) {
		if (MatchesTableSpec(table, spec)) {
			return true;
		}
	}
	return false;
}

std::string TableCreateSql(TableCatalogEntry &table) {
	auto table_info = table.GetInfo();
	table_info->catalog.clear();
	return table_info->ToString();
}

int64_t EstimatedRows(ClientContext &context, TableCatalogEntry &table) {
	try {
		auto storage_info = table.GetStorageInfo(context);
		if (storage_info.cardinality.IsValid()) {
			return static_cast<int64_t>(storage_info.cardinality.GetIndex());
		}
	} catch (std::exception &ex) {
	}
	return -1;
}

vector<SchemaColumnInfo> ExtractSchemaColumns(TableCatalogEntry &table) {
	std::set<idx_t> not_null_cols;
	for (auto &constraint : table.GetConstraints()) {
		if (constraint->type == ConstraintType::NOT_NULL) {
			auto &not_null = constraint->Cast<NotNullConstraint>();
			not_null_cols.insert(not_null.index.index);
		}
	}

	vector<SchemaColumnInfo> columns;
	auto column_count = table.GetColumns().LogicalColumnCount();
	columns.reserve(column_count);
	for (idx_t i = 0; i < column_count; i++) {
		auto &column = table.GetColumn(LogicalIndex(i));
		SchemaColumnInfo info;
		info.name = column.Name();
		info.type = column.Type().ToString();
		info.nullable = not_null_cols.find(i) == not_null_cols.end();
		if (column.Generated()) {
			info.default_value = column.GeneratedExpression().ToString();
		} else if (column.HasDefaultValue()) {
			info.default_value = column.DefaultValue().ToString();
		}
		info.comment = ValueToOptionalString(column.Comment());
		columns.push_back(std::move(info));
	}
	return columns;
}

std::string NormalizeSampleValue(std::string value) {
	for (auto &c : value) {
		if (c == '\n' || c == '\r' || c == '\t') {
			c = ' ';
		}
	}
	if (value.size() > 160) {
		value = value.substr(0, 157) + "...";
	}
	return value;
}

std::string SampleValueToString(const Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	return NormalizeSampleValue(value.ToString());
}

bool TryCollectSampleRows(ClientContext &context, TableCatalogEntry &table, SchemaTableInfo &info, int64_t sample_rows,
                          bool allow_actual_samples) {
	if (sample_rows <= 0 || !allow_actual_samples) {
		return false;
	}
	if (!table.IsDuckTable()) {
		return false;
	}

	try {
		vector<StorageIndex> column_ids;
		vector<LogicalType> column_types;
		vector<std::string> column_names;
		auto column_count = table.GetColumns().LogicalColumnCount();
		for (idx_t i = 0; i < column_count; i++) {
			auto &column = table.GetColumn(LogicalIndex(i));
			if (column.Generated()) {
				continue;
			}
			column_ids.push_back(table.GetStorageIndex(ColumnIndex(i)));
			column_types.push_back(column.Type());
			column_names.push_back(column.Name());
		}
		if (column_ids.empty()) {
			return false;
		}

		auto &storage = table.GetStorage();
		auto &transaction = DuckTransaction::Get(context, table.catalog);
		TableScanState scan_state;
		storage.InitializeScan(context, transaction, scan_state, column_ids);

		DataChunk chunk;
		chunk.Initialize(context, column_types);
		auto remaining_rows = NumericCast<idx_t>(sample_rows);
		while (remaining_rows > 0) {
			chunk.Reset();
			storage.Scan(transaction, chunk, scan_state);
			if (chunk.size() == 0) {
				break;
			}
			auto chunk_rows = MinValue<idx_t>(chunk.size(), remaining_rows);
			for (idx_t row = 0; row < chunk_rows; row++) {
				SchemaSampleRow sample;
				for (idx_t col = 0; col < column_names.size(); col++) {
					sample.values.emplace_back(column_names[col], SampleValueToString(chunk.GetValue(col, row)));
				}
				info.sample_rows.push_back(std::move(sample));
			}
			remaining_rows -= chunk_rows;
		}
		return true;
	} catch (std::exception &ex) {
		info.sample_error = ex.what();
		return false;
	}
}

std::string BuildTableSampleHint(const SchemaTableInfo &table, int64_t sample_rows) {
	if (sample_rows <= 0) {
		return "";
	}
	return "  sample_query: SELECT * FROM " + QualifiedTableName(table) + " LIMIT " + std::to_string(sample_rows) +
	       "\n";
}

std::string BuildPromptSchemaContext(ClientContext &context, const PromptSchemaOptions &options,
                                     bool allow_actual_samples = false) {
	auto include_specs = ParseTableSpecs(options.include_tables, "include_tables");
	auto exclude_specs = ParseTableSpecs(options.exclude_tables, "exclude_tables");
	auto current_database = DatabaseManager::GetDefaultDatabase(context);
	auto current_database_lower = LowerAscii(current_database);
	vector<SchemaTableInfo> tables;

	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.type != CatalogType::TABLE_ENTRY || entry.internal) {
				return;
			}
			auto &table = entry.Cast<TableCatalogEntry>();
			if (!table.temporary && LowerAscii(table.catalog.GetName()) != current_database_lower) {
				return;
			}

			SchemaTableInfo info;
			info.catalog = table.catalog.GetName();
			info.schema = table.schema.name;
			info.table = table.name;
			info.comment = ValueToOptionalString(table.comment);
			info.sql = TableCreateSql(table);
			info.estimated_rows = EstimatedRows(context, table);
			info.columns = ExtractSchemaColumns(table);
			TryCollectSampleRows(context, table, info, options.sample_rows, allow_actual_samples);
			auto included = include_specs.empty() || MatchesAnyTableSpec(info, include_specs);
			auto excluded = MatchesAnyTableSpec(info, exclude_specs);
			if (included && !excluded) {
				tables.push_back(std::move(info));
			}
		});
	}

	std::sort(tables.begin(), tables.end(), [](const SchemaTableInfo &left, const SchemaTableInfo &right) {
		if (left.catalog != right.catalog) {
			return left.catalog < right.catalog;
		}
		if (left.schema != right.schema) {
			return left.schema < right.schema;
		}
		return left.table < right.table;
	});

	std::ostringstream out;
	out << "DuckDB schema context\n";
	out << "Current database: " << QuoteSqlIdentifier(current_database) << "\n";
	if (!options.include_tables.empty()) {
		out << "Included tables:";
		for (auto &include_table : options.include_tables) {
			out << " " << include_table;
		}
		out << "\n";
	}
	if (!options.exclude_tables.empty()) {
		out << "Excluded tables:";
		for (auto &exclude_table : options.exclude_tables) {
			out << " " << exclude_table;
		}
		out << "\n";
	}
	if (options.sample_rows > 0) {
		out << "Sample row limit per table: " << options.sample_rows << "\n";
	}
	if (tables.empty()) {
		out << "No user tables found in current database.";
		return out.str();
	}

	out << "Tables:\n";
	for (auto &table : tables) {
		out << "- table: " << QualifiedTableName(table) << "\n";
		if (!table.comment.empty()) {
			out << "  comment: " << table.comment << "\n";
		}
		if (table.estimated_rows >= 0) {
			out << "  estimated_rows: " << table.estimated_rows << "\n";
		}
		out << "  create_sql: " << table.sql << "\n";
		out << "  columns:\n";
		for (auto &column : table.columns) {
			out << "  - " << QuoteSqlIdentifier(column.name) << " " << column.type;
			if (!column.nullable) {
				out << " NOT NULL";
			}
			if (!column.default_value.empty()) {
				out << " DEFAULT " << column.default_value;
			}
			if (!column.comment.empty()) {
				out << " -- " << column.comment;
			}
			out << "\n";
		}
		if (!table.sample_rows.empty()) {
			out << "  sample_rows:\n";
			for (auto &row : table.sample_rows) {
				out << "  -";
				for (auto &value : row.values) {
					out << " " << QuoteSqlIdentifier(value.first) << "=" << value.second << ";";
				}
				out << "\n";
			}
		} else {
			out << BuildTableSampleHint(table, options.sample_rows);
			if (!table.sample_error.empty()) {
				out << "  sample_error: " << NormalizeSampleValue(table.sample_error) << "\n";
			}
		}
	}
	return out.str();
}

void AiTaskFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiTaskBindData>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<string_t>(result);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader input_reader(args, 0);
	auto parameter_reader = OptionalStringReader(args, bind_data.required_args == 2, 1);
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		if (!input_reader.Read(row, input)) {
			result_validity.SetInvalid(row);
			continue;
		}
		std::string parameter;
		if (bind_data.required_args == 2 && !parameter_reader->Read(row, parameter)) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecret(state.GetContext(), options);
			auto prompt = BuildTaskPrompt(bind_data.task, input, parameter);
			auto output = duckdb_ai::Complete(prompt, options).text;
			result_data[row] = StringVector::AddString(result, output);
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}
}

bool ParseAiBooleanResult(const std::string &input, bool &value) {
	auto normalized = LowerAscii(TrimAscii(input));
	while (!normalized.empty() && (normalized.back() == '.' || normalized.back() == '!' || normalized.back() == ',')) {
		normalized.pop_back();
	}
	if (normalized == "true" || normalized == "yes" || normalized == "y" || normalized == "1") {
		value = true;
		return true;
	}
	if (normalized == "false" || normalized == "no" || normalized == "n" || normalized == "0") {
		value = false;
		return true;
	}
	return false;
}

void AiFilterFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiTaskBindData>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<bool>(result);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader input_reader(args, 0);
	StringVectorReader predicate_reader(args, 1);
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		std::string predicate;
		if (!input_reader.Read(row, input) || !predicate_reader.Read(row, predicate)) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecret(state.GetContext(), options);
			auto prompt = BuildTaskPrompt(bind_data.task, input, predicate);
			auto output = duckdb_ai::Complete(prompt, options).text;
			bool parsed = false;
			if (!ParseAiBooleanResult(output, parsed)) {
				throw InvalidInputException("ai_filter expected true or false output, got: %s", output);
			}
			result_data[row] = parsed;
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}
}

unique_ptr<FunctionData> AiCountTokensBind(ClientContext &context, ScalarFunction &bound_function,
                                           vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = AiCompletionBindInternal(context, bound_function, arguments, false);
	bound_function.SetReturnType(LogicalType::BIGINT);
	return bind_data;
}

int64_t EstimateTokenCount(const std::string &input) {
	if (input.empty()) {
		return 0;
	}
	return static_cast<int64_t>((input.size() + 3) / 4);
}

void AiCountTokensFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiCompletionBindData>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<int64_t>(result);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader input_reader(args, bind_data.prompt_index);

	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		if (!input_reader.Read(row, input)) {
			result_validity.SetInvalid(row);
			continue;
		}
		result_data[row] = EstimateTokenCount(input);
	}
}

bool ValidateReadOnlySql(const std::string &sql, std::string &error) {
	Parser parser;
	try {
		parser.ParseQuery(sql);
	} catch (std::exception &ex) {
		error = ex.what();
		return false;
	}

	if (parser.statements.size() != 1) {
		error = "expected exactly one SQL statement";
		return false;
	}
	auto statement_type = parser.statements[0]->type;
	if (statement_type != StatementType::SELECT_STATEMENT) {
		error = "expected a SELECT statement, got " + StatementTypeToString(statement_type);
		return false;
	}
	return true;
}

void RequireReadOnlySql(const std::string &sql, const std::string &function_name) {
	std::string error;
	if (!ValidateReadOnlySql(sql, error)) {
		throw InvalidInputException("%s requires a single read-only SELECT statement: %s", function_name, error);
	}
}

std::string GeneratePromptSql(const std::string &question, const std::string &schema_context,
                              const duckdb_ai::CompletionOptions &options) {
	auto prompt = BuildPromptSqlPrompt(question, schema_context);
	auto generated_sql = StripMarkdownSqlFence(duckdb_ai::Complete(prompt, options).text);
	std::string error;
	if (!ValidateReadOnlySql(generated_sql, error)) {
		throw InvalidInputException("AI generated SQL is not a single read-only SELECT statement: %s", error);
	}
	return generated_sql;
}

std::string BuildPromptExplainPrompt(const std::string &sql, const std::string &schema_context) {
	std::string prompt =
	    "Explain the DuckDB SQL query below in plain English.\n"
	    "Rules:\n"
	    "- Explain what the query returns, the main clauses, joins, filters, grouping, ordering, and limits.\n"
	    "- Mention assumptions or likely issues if the SQL is ambiguous.\n"
	    "- Do not rewrite or execute the query.\n";
	if (!schema_context.empty()) {
		prompt += "\nAvailable schema/context:\n";
		prompt += schema_context;
		prompt += "\n";
	}
	prompt += "\nSQL query:\n";
	prompt += sql;
	return prompt;
}

std::string BuildPromptFixupPrompt(const std::string &sql, const std::string &schema_context) {
	std::string prompt = "Correct the DuckDB SQL query below.\n"
	                     "Rules:\n"
	                     "- Return only one corrected DuckDB SQL SELECT statement.\n"
	                     "- Do not wrap the answer in markdown or code fences.\n"
	                     "- Preserve the user's intent as closely as possible.\n"
	                     "- The corrected SQL must be read-only and must contain exactly one SELECT statement.\n";
	if (!schema_context.empty()) {
		prompt += "\nAvailable schema/context:\n";
		prompt += schema_context;
		prompt += "\n";
	}
	prompt += "\nBroken SQL query:\n";
	prompt += sql;
	return prompt;
}

idx_t ExtractLineNumber(const std::string &error_message) {
	auto lower = LowerAscii(error_message);
	auto pos = lower.find("line ");
	while (pos != std::string::npos) {
		pos += 5;
		while (pos < lower.size() && std::isspace(static_cast<unsigned char>(lower[pos]))) {
			pos++;
		}
		auto start = pos;
		while (pos < lower.size() && std::isdigit(static_cast<unsigned char>(lower[pos]))) {
			pos++;
		}
		if (pos > start) {
			return static_cast<idx_t>(std::stoull(lower.substr(start, pos - start)));
		}
		pos = lower.find("line ", pos);
	}
	return 1;
}

std::string BuildPromptFixLinePrompt(const std::string &sql, const std::string &error_message,
                                     const std::string &schema_context, idx_t line_number) {
	std::string prompt = "Correct one line in the DuckDB SQL query below.\n"
	                     "Rules:\n"
	                     "- Return only the corrected replacement line text.\n"
	                     "- Do not return the full query.\n"
	                     "- Do not wrap the answer in markdown or code fences.\n";
	if (!schema_context.empty()) {
		prompt += "\nAvailable schema/context:\n";
		prompt += schema_context;
		prompt += "\n";
	}
	if (!error_message.empty()) {
		prompt += "\nError message:\n";
		prompt += error_message;
		prompt += "\n";
	}
	prompt += "\nLine to correct: ";
	prompt += std::to_string(line_number);
	prompt += "\nSQL query:\n";
	prompt += sql;
	return prompt;
}

std::string GeneratePromptFixupSql(const std::string &sql, const std::string &schema_context,
                                   const duckdb_ai::CompletionOptions &options) {
	auto prompt = BuildPromptFixupPrompt(sql, schema_context);
	auto fixed_sql = StripMarkdownSqlFence(duckdb_ai::Complete(prompt, options).text);
	std::string error;
	if (!ValidateReadOnlySql(fixed_sql, error)) {
		throw InvalidInputException("AI corrected SQL is not a single read-only SELECT statement: %s", error);
	}
	return fixed_sql;
}

unique_ptr<TableRef> ParseReadOnlySubquery(const std::string &sql, const ParserOptions &options) {
	Parser parser(options);
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1) {
		throw ParserException("AI generated SQL must be exactly one SELECT statement");
	}
	auto &statement = parser.statements[0];
	if (statement->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("AI generated SQL must be a SELECT statement");
	}
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(statement));
	return make_uniq<SubqueryRef>(std::move(select));
}

std::string RequiredTableStringInput(const TableFunctionBindInput &input, idx_t index, const std::string &name) {
	if (index >= input.inputs.size()) {
		throw BinderException("ai_query_data requires a %s argument", name);
	}
	auto &value = input.inputs[index];
	if (value.IsNull()) {
		throw BinderException("ai_query_data %s argument must not be NULL", name);
	}
	return StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
}

std::string OptionalTableStringInput(const TableFunctionBindInput &input, idx_t index) {
	if (index >= input.inputs.size()) {
		return "";
	}
	auto &value = input.inputs[index];
	if (value.IsNull()) {
		throw BinderException("ai_query_data positional options must not be NULL");
	}
	return StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
}

void ApplyPromptQueryValueOption(duckdb_ai::CompletionOptions &options, std::string &schema_context,
                                 PromptSchemaOptions &schema_options, const std::string &name, const Value &value_p) {
	if (value_p.IsNull()) {
		throw BinderException("ai_query_data option \"%s\" must not be NULL", name);
	}
	auto value = value_p;
	if (name == "schema_context" || name == "schema") {
		schema_context = StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
		return;
	}
	if (name == "include_tables") {
		schema_options.include_tables = ReadIncludeTablesValue(value, "include_tables", "ai_query_data");
		return;
	}
	if (name == "exclude_tables") {
		schema_options.exclude_tables = ReadIncludeTablesValue(value, "exclude_tables", "ai_query_data");
		return;
	}
	if (name == "sample_rows") {
		schema_options.sample_rows = ReadSampleRowsValue(value, "ai_query_data");
		return;
	}
	if (!ApplyCompletionValueOption(options, "ai_query_data", name, value_p, true, false)) {
		throw BinderException("Unsupported ai_query_data option \"%s\"", name);
	}
}

unique_ptr<TableRef> EmptyPromptQueryResult(const ParserOptions &options) {
	return ParseReadOnlySubquery("SELECT NULL::VARCHAR AS ai_query_data_error WHERE FALSE", options);
}

unique_ptr<TableRef> PromptQueryBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto options = duckdb_ai::CompletionOptions();
	ApplySettings(context, options);
	auto question = RequiredTableStringInput(input, 0, "question");
	auto schema_context = OptionalTableStringInput(input, 1);
	PromptSchemaOptions schema_options;
	if (input.inputs.size() > 2) {
		options.model = OptionalTableStringInput(input, 2);
	}
	if (input.inputs.size() > 3) {
		options.provider = OptionalTableStringInput(input, 3);
	}
	for (auto &named_parameter : input.named_parameters) {
		ApplyPromptQueryValueOption(options, schema_context, schema_options, LowerAscii(named_parameter.first),
		                            named_parameter.second);
	}
	if (schema_context.empty()) {
		schema_context = BuildPromptSchemaContext(context, schema_options);
	}

	try {
		ApplyAiProviderSecret(context, options);
		auto generated_sql = GeneratePromptSql(question, schema_context, options);
		return ParseReadOnlySubquery(generated_sql, context.GetParserOptions());
	} catch (std::exception &ex) {
		if (options.fail_on_error) {
			throw;
		}
		return EmptyPromptQueryResult(context.GetParserOptions());
	}
}

void AiPromptSqlFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiPromptSqlBindData>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<string_t>(result);
	auto &result_validity = FlatVector::Validity(result);
	std::string auto_schema_context;
	bool has_auto_schema_context = false;
	StringVectorReader question_reader(args, 0);
	auto schema_context_reader =
	    OptionalStringReader(args, bind_data.has_schema_context_arg, bind_data.schema_context_index);
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	for (idx_t row = 0; row < args.size(); row++) {
		std::string question;
		if (!question_reader.Read(row, question)) {
			result_validity.SetInvalid(row);
			continue;
		}

		auto schema_context = bind_data.schema_context;
		if (bind_data.has_schema_context_arg && !schema_context_reader->Read(row, schema_context)) {
			result_validity.SetInvalid(row);
			continue;
		}
		if (schema_context.empty()) {
			if (!has_auto_schema_context) {
				auto_schema_context = BuildPromptSchemaContext(state.GetContext(), bind_data.schema_options, true);
				has_auto_schema_context = true;
			}
			schema_context = auto_schema_context;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecret(state.GetContext(), options);
			auto generated_sql = GeneratePromptSql(question, schema_context, options);
			result_data[row] = StringVector::AddString(result, generated_sql);
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}
}

vector<std::string> ReadIncludeTablesValue(const Value &value_p, const std::string &name,
                                           const std::string &function_name) {
	if (value_p.IsNull()) {
		throw BinderException("%s %s must not be NULL", function_name, name);
	}
	auto value = value_p;
	if (value.type().id() == LogicalTypeId::LIST) {
		value = value.DefaultCastAs(LogicalType::LIST(LogicalType::VARCHAR));
		vector<std::string> result;
		for (auto &child : ListValue::GetChildren(value)) {
			if (child.IsNull()) {
				throw BinderException("%s %s entries must not be NULL", function_name, name);
			}
			auto child_value = child;
			result.push_back(StringValue::Get(child_value.DefaultCastAs(LogicalType::VARCHAR)));
		}
		return result;
	}
	return {StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR))};
}

int64_t ReadSampleRowsValue(const Value &value_p, const std::string &function_name) {
	if (value_p.IsNull()) {
		throw BinderException("%s sample_rows must not be NULL", function_name);
	}
	auto value = value_p;
	auto sample_rows = BigIntValue::Get(value.DefaultCastAs(LogicalType::BIGINT));
	if (sample_rows < 0 || sample_rows > 100) {
		throw BinderException("%s sample_rows must be between 0 and 100", function_name);
	}
	return sample_rows;
}

PromptSchemaOptions PromptSchemaOptionsFromInput(TableFunctionBindInput &input) {
	PromptSchemaOptions options;
	bool has_include_tables = false;
	if (input.inputs.size() > 1) {
		throw BinderException("ai_schema_prompt supports at most one positional include_tables argument");
	}
	if (!input.inputs.empty()) {
		options.include_tables = ReadIncludeTablesValue(input.inputs[0], "include_tables", "ai_schema_prompt");
		has_include_tables = true;
	}
	for (auto &named_parameter : input.named_parameters) {
		auto name = LowerAscii(named_parameter.first);
		if (name == "include_tables") {
			if (has_include_tables) {
				throw BinderException(
				    "ai_schema_prompt include_tables cannot be supplied both positionally and by name");
			}
			options.include_tables =
			    ReadIncludeTablesValue(named_parameter.second, "include_tables", "ai_schema_prompt");
			has_include_tables = true;
			continue;
		}
		if (name == "exclude_tables") {
			options.exclude_tables =
			    ReadIncludeTablesValue(named_parameter.second, "exclude_tables", "ai_schema_prompt");
			continue;
		}
		if (name == "sample_rows") {
			options.sample_rows = ReadSampleRowsValue(named_parameter.second, "ai_schema_prompt");
			continue;
		}
		throw BinderException("Unsupported ai_schema_prompt option \"%s\"", name);
	}
	return options;
}

unique_ptr<FunctionData> PromptSchemaBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("summary");
	return_types.emplace_back(LogicalType::VARCHAR);
	auto options = PromptSchemaOptionsFromInput(input);
	return make_uniq<PromptSchemaBindData>(std::move(options));
}

unique_ptr<GlobalTableFunctionState> PromptSchemaInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PromptSchemaScanData>();
}

void PromptSchemaFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<PromptSchemaScanData>();
	if (state.emitted) {
		return;
	}
	auto &bind_data = input.bind_data->Cast<PromptSchemaBindData>();
	auto summary = BuildPromptSchemaContext(context, bind_data.options, true);
	output.SetValue(0, 0, Value(summary));
	output.SetCardinality(1);
	state.emitted = true;
	duckdb_ai::RecordLocalUsageEvent("ai_schema_prompt", 0, static_cast<int64_t>(summary.size()));
}

std::string RequiredAssistantStringInput(const TableFunctionBindInput &input, idx_t index, const std::string &name,
                                         const std::string &function_name) {
	if (index >= input.inputs.size()) {
		throw BinderException("%s requires a %s argument", function_name, name);
	}
	auto &value = input.inputs[index];
	if (value.IsNull()) {
		throw BinderException("%s %s argument must not be NULL", function_name, name);
	}
	return StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
}

std::string OptionalAssistantStringInput(const TableFunctionBindInput &input, idx_t index,
                                         const std::string &function_name) {
	if (index >= input.inputs.size()) {
		return "";
	}
	auto &value = input.inputs[index];
	if (value.IsNull()) {
		throw BinderException("%s positional options must not be NULL", function_name);
	}
	return StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
}

void ApplyPromptAssistantValueOption(PromptAssistantBindData &bind_data, PromptSchemaOptions &schema_options,
                                     bool &has_include_tables, bool &has_error, const std::string &function_name,
                                     const std::string &name, const Value &value_p) {
	if (value_p.IsNull()) {
		throw BinderException("%s option \"%s\" must not be NULL", function_name, name);
	}
	auto value = value_p;
	if (name == "schema_context" || name == "schema") {
		bind_data.schema_context = StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
		return;
	}
	if (name == "include_tables") {
		if (has_include_tables) {
			throw BinderException("%s include_tables cannot be supplied more than once", function_name);
		}
		schema_options.include_tables = ReadIncludeTablesValue(value, "include_tables", function_name);
		has_include_tables = true;
		return;
	}
	if (name == "exclude_tables") {
		schema_options.exclude_tables = ReadIncludeTablesValue(value, "exclude_tables", function_name);
		return;
	}
	if (name == "sample_rows") {
		schema_options.sample_rows = ReadSampleRowsValue(value, function_name);
		return;
	}
	if (name == "error") {
		if (bind_data.kind != PromptAssistantKind::FIX_LINE) {
			throw BinderException("%s does not support an error option", function_name);
		}
		if (has_error) {
			throw BinderException("%s error cannot be supplied more than once", function_name);
		}
		bind_data.error_message = StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
		has_error = true;
		return;
	}
	if (!ApplyCompletionValueOption(bind_data.options, function_name, name, value_p, true, false)) {
		throw BinderException("Unsupported %s option \"%s\"", function_name, name);
	}
}

unique_ptr<FunctionData> PromptAssistantBindInternal(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names,
                                                     PromptAssistantKind kind, const std::string &function_name) {
	auto bind_data = make_uniq<PromptAssistantBindData>(kind);
	ApplySettings(context, bind_data->options);
	auto max_positional_args = kind == PromptAssistantKind::FIX_LINE ? 2 : 1;
	if (input.inputs.empty() || input.inputs.size() > max_positional_args) {
		throw BinderException("%s expects %s", function_name,
		                      kind == PromptAssistantKind::FIX_LINE ? "one SQL argument and optional error argument"
		                                                            : "one SQL argument");
	}
	bind_data->sql = RequiredAssistantStringInput(input, 0, "sql", function_name);

	bool has_error = false;
	if (kind == PromptAssistantKind::FIX_LINE && input.inputs.size() > 1) {
		bind_data->error_message = OptionalAssistantStringInput(input, 1, function_name);
		has_error = true;
	}

	PromptSchemaOptions schema_options;
	bool has_include_tables = false;
	for (auto &named_parameter : input.named_parameters) {
		ApplyPromptAssistantValueOption(*bind_data, schema_options, has_include_tables, has_error, function_name,
		                                LowerAscii(named_parameter.first), named_parameter.second);
	}
	if (bind_data->schema_context.empty()) {
		bind_data->schema_context = BuildPromptSchemaContext(context, schema_options);
	}

	if (kind == PromptAssistantKind::EXPLAIN) {
		names.emplace_back("explanation");
		return_types.emplace_back(LogicalType::VARCHAR);
	} else if (kind == PromptAssistantKind::FIXUP) {
		names.emplace_back("sql");
		return_types.emplace_back(LogicalType::VARCHAR);
	} else {
		names.emplace_back("line_number");
		return_types.emplace_back(LogicalType::BIGINT);
		names.emplace_back("replacement_line");
		return_types.emplace_back(LogicalType::VARCHAR);
	}
	return std::move(bind_data);
}

unique_ptr<FunctionData> PromptExplainBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	return PromptAssistantBindInternal(context, input, return_types, names, PromptAssistantKind::EXPLAIN,
	                                   "ai_explain_sql");
}

unique_ptr<FunctionData> PromptFixupBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	return PromptAssistantBindInternal(context, input, return_types, names, PromptAssistantKind::FIXUP, "ai_fix_sql");
}

unique_ptr<FunctionData> PromptFixLineBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	return PromptAssistantBindInternal(context, input, return_types, names, PromptAssistantKind::FIX_LINE,
	                                   "ai_fix_sql_line");
}

unique_ptr<GlobalTableFunctionState> PromptAssistantInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PromptAssistantScanData>();
}

void PromptAssistantNullRow(const PromptAssistantBindData &bind_data, DataChunk &output) {
	if (bind_data.kind == PromptAssistantKind::FIX_LINE) {
		output.SetValue(0, 0, Value());
		output.SetValue(1, 0, Value());
	} else {
		output.SetValue(0, 0, Value());
	}
	output.SetCardinality(1);
}

void PromptAssistantFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<PromptAssistantScanData>();
	if (state.emitted) {
		return;
	}
	auto &bind_data = input.bind_data->Cast<PromptAssistantBindData>();
	try {
		auto options = bind_data.options;
		ApplyAiProviderSecret(context, options);
		if (bind_data.kind == PromptAssistantKind::EXPLAIN) {
			RequireReadOnlySql(bind_data.sql, "ai_explain_sql");
			auto prompt = BuildPromptExplainPrompt(bind_data.sql, bind_data.schema_context);
			auto explanation = duckdb_ai::Complete(prompt, options).text;
			output.SetValue(0, 0, Value(explanation));
		} else if (bind_data.kind == PromptAssistantKind::FIXUP) {
			auto fixed_sql = GeneratePromptFixupSql(bind_data.sql, bind_data.schema_context, options);
			output.SetValue(0, 0, Value(fixed_sql));
		} else {
			auto line_number = ExtractLineNumber(bind_data.error_message);
			auto prompt =
			    BuildPromptFixLinePrompt(bind_data.sql, bind_data.error_message, bind_data.schema_context, line_number);
			auto replacement = StripMarkdownSqlFence(duckdb_ai::Complete(prompt, options).text);
			output.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(line_number)));
			output.SetValue(1, 0, Value(TrimAscii(replacement)));
		}
		output.SetCardinality(1);
	} catch (std::exception &ex) {
		if (bind_data.options.fail_on_error) {
			throw;
		}
		PromptAssistantNullRow(bind_data, output);
	}
	state.emitted = true;
}

void AiIsReadOnlySqlFunction(DataChunk &args, ExpressionState &, Vector &result) {
	auto &sql_vector = args.data[0];
	UnaryExecutor::Execute<string_t, bool>(sql_vector, result, args.size(), [&](string_t sql) {
		std::string error;
		return ValidateReadOnlySql(sql.GetString(), error);
	});
}

void AiValidateReadOnlySqlFunction(DataChunk &args, ExpressionState &, Vector &result) {
	auto &sql_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(sql_vector, result, args.size(), [&](string_t sql) {
		auto sql_string = sql.GetString();
		std::string error;
		if (!ValidateReadOnlySql(sql_string, error)) {
			throw InvalidInputException("AI generated SQL is not a single read-only SELECT statement: %s", error);
		}
		return StringVector::AddString(result, sql_string);
	});
}

inline void AiProviderBaseUrl(DataChunk &args, ExpressionState &, Vector &result) {
	auto &provider_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(provider_vector, result, args.size(), [&](string_t provider) {
		return StringVector::AddString(result, duckdb_ai::ProviderBaseUrl(provider.GetString()));
	});
}

inline void AiProviderProtocol(DataChunk &args, ExpressionState &, Vector &result) {
	auto &provider_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(provider_vector, result, args.size(), [&](string_t provider) {
		return StringVector::AddString(result, duckdb_ai::ProviderProtocol(provider.GetString()));
	});
}

std::string ScopeString(const vector<std::string> &scope) {
	std::string result;
	for (idx_t i = 0; i < scope.size(); i++) {
		if (i > 0) {
			result += ",";
		}
		result += scope[i];
	}
	return result;
}

bool SecretHasNonEmptyValue(const KeyValueSecret &secret, const std::string &key) {
	std::string value;
	return TryReadSecretString(secret, key, value);
}

unique_ptr<FunctionData> AiSecretsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                       vector<string> &names) {
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("provider");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("model");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("base_url");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("scope");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("storage");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("persistent");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("has_api_key");
	return_types.emplace_back(LogicalType::BOOLEAN);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> AiSecretsInit(ClientContext &context, TableFunctionInitInput &) {
	auto result = make_uniq<AiSecretsScanData>();
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	result->secrets = SecretManager::Get(context).AllSecrets(transaction);
	return std::move(result);
}

void AiSecretsFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<AiSecretsScanData>();
	idx_t count = 0;
	while (data.offset < data.secrets.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.secrets[data.offset++];
		if (!entry.secret || entry.secret->GetType() != "duckdb_ai") {
			continue;
		}
		auto secret = dynamic_cast<const KeyValueSecret *>(entry.secret.get());
		if (!secret) {
			continue;
		}
		std::string provider;
		std::string model;
		std::string base_url;
		TryReadSecretString(*secret, "provider", provider);
		TryReadSecretString(*secret, "model", model);
		TryReadSecretString(*secret, "base_url", base_url);

		idx_t col = 0;
		output.SetValue(col++, count, Value(entry.secret->GetName()));
		output.SetValue(col++, count, provider.empty() ? Value() : Value(provider));
		output.SetValue(col++, count, model.empty() ? Value() : Value(model));
		output.SetValue(col++, count, base_url.empty() ? Value() : Value(base_url));
		output.SetValue(col++, count, Value(ScopeString(entry.secret->GetScope())));
		output.SetValue(col++, count, Value(entry.storage_mode));
		output.SetValue(col++, count, Value::BOOLEAN(entry.persist_type == SecretPersistType::PERSISTENT));
		output.SetValue(col++, count, Value::BOOLEAN(SecretHasNonEmptyValue(*secret, "api_key")));
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> AiUsageBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                     vector<string> &names) {
	names.emplace_back("event_id");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("created_at");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("event");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("provider");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("protocol");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("model");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("prompt_chars");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("response_chars");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("input_chars");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("dimensions");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("prompt_tokens");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("completion_tokens");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("total_tokens");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("elapsed_ms");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("http_status");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("estimated_cost_usd");
	return_types.emplace_back(LogicalType::DOUBLE);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> AiUsageInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<AiUsageScanData>();
}

void AiUsageFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<AiUsageScanData>();
	if (data.offset >= data.events.size()) {
		return;
	}

	idx_t count = 0;
	while (data.offset < data.events.size() && count < STANDARD_VECTOR_SIZE) {
		auto &event = data.events[data.offset++];
		idx_t col = 0;
		output.SetValue(col++, count, Value::UBIGINT(event.event_id));
		output.SetValue(col++, count, Value(event.created_at));
		output.SetValue(col++, count, Value(event.event));
		output.SetValue(col++, count, Value(event.provider));
		output.SetValue(col++, count, Value(event.protocol));
		output.SetValue(col++, count, Value(event.model));
		output.SetValue(col++, count, Value::BIGINT(event.prompt_chars));
		output.SetValue(col++, count, Value::BIGINT(event.response_chars));
		output.SetValue(col++, count, Value::BIGINT(event.input_chars));
		output.SetValue(col++, count, Value::BIGINT(event.dimensions));
		output.SetValue(col++, count, Value::BIGINT(event.prompt_tokens));
		output.SetValue(col++, count, Value::BIGINT(event.completion_tokens));
		output.SetValue(col++, count, Value::BIGINT(event.total_tokens));
		output.SetValue(col++, count, Value::BIGINT(event.elapsed_ms));
		output.SetValue(col++, count, Value::BIGINT(event.http_status));
		if (event.estimated_cost_usd >= 0 && std::isfinite(event.estimated_cost_usd)) {
			output.SetValue(col++, count, Value::DOUBLE(event.estimated_cost_usd));
		} else {
			output.SetValue(col++, count, Value());
		}
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> AiClearUsageBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                          vector<string> &names) {
	names.emplace_back("cleared");
	return_types.emplace_back(LogicalType::BOOLEAN);
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> AiClearUsageInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<AiClearUsageScanData>();
}

void AiClearUsageFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<AiClearUsageScanData>();
	if (data.emitted) {
		return;
	}
	duckdb_ai::ClearUsageEvents();
	output.SetValue(0, 0, Value::BOOLEAN(true));
	output.SetCardinality(1);
	data.emitted = true;
}

unique_ptr<FunctionData> AiModelPricesBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           vector<string> &names) {
	names.emplace_back("provider");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("model");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("operation");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("input_token_price_per_million");
	return_types.emplace_back(LogicalType::DOUBLE);

	names.emplace_back("output_token_price_per_million");
	return_types.emplace_back(LogicalType::DOUBLE);

	names.emplace_back("source_url");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("source_note");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("last_reviewed");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> AiModelPricesInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<AiModelPricesScanData>();
}

void AiModelPricesFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<AiModelPricesScanData>();
	if (data.offset >= data.prices.size()) {
		return;
	}

	idx_t count = 0;
	while (data.offset < data.prices.size() && count < STANDARD_VECTOR_SIZE) {
		auto &price = data.prices[data.offset++];
		idx_t col = 0;
		output.SetValue(col++, count, Value(price.provider));
		output.SetValue(col++, count, Value(price.model));
		output.SetValue(col++, count, Value(price.operation));
		output.SetValue(col++, count, Value::DOUBLE(price.input_token_price_per_million));
		if (price.output_token_price_per_million >= 0 && std::isfinite(price.output_token_price_per_million)) {
			output.SetValue(col++, count, Value::DOUBLE(price.output_token_price_per_million));
		} else {
			output.SetValue(col++, count, Value());
		}
		output.SetValue(col++, count, Value(price.source_url));
		output.SetValue(col++, count, Value(price.source_note));
		output.SetValue(col++, count, Value(price.last_reviewed));
		count++;
	}
	output.SetCardinality(count);
}

void AddSetting(DBConfig &config, const std::string &name, const std::string &description, LogicalType type,
                const Value &default_value) {
	if (config.HasExtensionOption(name)) {
		return;
	}
	config.AddExtensionOption(name, description, std::move(type), default_value);
}

void RegisterSettingsForPrefix(DBConfig &config, const std::string &prefix, const std::string &label) {
	AddSetting(config, prefix + "_provider", "Default AI provider for " + label, LogicalType::VARCHAR, Value(""));
	AddSetting(config, prefix + "_model", "Default AI model for " + label, LogicalType::VARCHAR, Value(""));
	AddSetting(config, prefix + "_base_url", "Default AI provider base URL override for " + label, LogicalType::VARCHAR,
	           Value(""));
	AddSetting(config, prefix + "_response_format", "Default AI response format: text, json_object, or json_schema",
	           LogicalType::VARCHAR, Value(""));
	AddSetting(config, prefix + "_response_schema", "Default AI JSON schema object for structured responses",
	           LogicalType::VARCHAR, Value(""));
	AddSetting(config, prefix + "_timeout_seconds", "AI provider HTTP timeout in seconds; 0 uses the extension default",
	           LogicalType::BIGINT, Value::BIGINT(0));
	AddSetting(config, prefix + "_retry_count", "AI provider retry count between 0 and 10; -1 uses default",
	           LogicalType::BIGINT, Value::BIGINT(-1));
	AddSetting(config, prefix + "_retry_backoff_ms",
	           "AI provider retry backoff in milliseconds between 0 and 60000; -1 uses default", LogicalType::BIGINT,
	           Value::BIGINT(-1));
	AddSetting(config, prefix + "_max_concurrent_requests",
	           "Maximum concurrent AI provider requests between 0 and 1024; 0 disables the limit, -1 uses default",
	           LogicalType::BIGINT, Value::BIGINT(-1));
	AddSetting(config, prefix + "_min_request_interval_ms",
	           "Minimum milliseconds between AI provider request starts between 0 and 60000; -1 uses default",
	           LogicalType::BIGINT, Value::BIGINT(-1));
	AddSetting(config, prefix + "_input_token_price_per_million",
	           "Input token price per million tokens for estimated AI usage cost; -1 disables cost estimates",
	           LogicalType::DOUBLE, Value::DOUBLE(-1));
	AddSetting(config, prefix + "_output_token_price_per_million",
	           "Output token price per million tokens for estimated AI usage cost; -1 disables cost estimates",
	           LogicalType::DOUBLE, Value::DOUBLE(-1));
	AddSetting(config, prefix + "_use_builtin_model_prices",
	           "Use " + label + " built-in model price catalog for estimated AI usage cost", LogicalType::BOOLEAN,
	           Value(LogicalType::BOOLEAN));
	AddSetting(config, prefix + "_log_endpoint", "HTTP endpoint for privacy-minimized AI usage logs",
	           LogicalType::VARCHAR, Value(""));
	AddSetting(config, prefix + "_log_format", "AI usage log payload format: generic_json or otlp_json",
	           LogicalType::VARCHAR, Value(""));
	AddSetting(config, prefix + "_log_tags", "Optional tag string included in AI usage logs", LogicalType::VARCHAR,
	           Value(""));
	AddSetting(config, prefix + "_log_sample_rate", "AI usage log sampling rate between 0 and 1; -1 uses default",
	           LogicalType::DOUBLE, Value::DOUBLE(-1));
	AddSetting(config, prefix + "_log_include_text", "Include prompt and response text in AI usage logs",
	           LogicalType::BOOLEAN, Value(LogicalType::BOOLEAN));
	AddSetting(config, prefix + "_log_strict", "Fail SQL queries when AI usage log delivery fails",
	           LogicalType::BOOLEAN, Value(LogicalType::BOOLEAN));
}

void RegisterSettings(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	RegisterSettingsForPrefix(config, "duckdb_ai", "duckdb_ai");
}

void RegisterAiProviderSecretType(ExtensionLoader &loader, const std::string &type_name) {
	SecretType secret_type;
	secret_type.name = type_name;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	secret_type.extension = "duckdb_ai";
	loader.RegisterSecretType(std::move(secret_type));

	CreateSecretFunction config_function;
	config_function.secret_type = type_name;
	config_function.provider = "config";
	config_function.function = CreateAiProviderSecretFromConfig;
	config_function.named_parameters["ai_provider"] = LogicalType::VARCHAR;
	config_function.named_parameters["api_key"] = LogicalType::VARCHAR;
	config_function.named_parameters["model"] = LogicalType::VARCHAR;
	config_function.named_parameters["base_url"] = LogicalType::VARCHAR;
	config_function.named_parameters["api_version"] = LogicalType::VARCHAR;
	loader.RegisterFunction(std::move(config_function));
}

void RegisterAiProviderSecret(ExtensionLoader &loader) {
	RegisterAiProviderSecretType(loader, "duckdb_ai");
}

void RegisterTaskFunction(ExtensionLoader &loader, const std::string &name, vector<LogicalType> arguments,
                          bind_scalar_function_t bind) {
	auto function = ScalarFunction(name, std::move(arguments), LogicalType::VARCHAR, AiTaskFunction, bind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(function);
}

void RegisterCompletionFunction(ExtensionLoader &loader, const std::string &name, bind_scalar_function_t bind) {
	auto function = ScalarFunction(name, {LogicalType::VARCHAR}, LogicalType::VARCHAR, AiCompletionFunction, bind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(function);
}

void RegisterEmbeddingFunction(ExtensionLoader &loader, const std::string &name) {
	auto function = ScalarFunction(name, {LogicalType::VARCHAR}, LogicalType::LIST(LogicalType::DOUBLE),
	                               AiEmbedFunction, AiEmbedBind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(function);
}

void RegisterAiFilterFunction(ExtensionLoader &loader) {
	auto function = ScalarFunction("ai_filter", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                               AiFilterFunction, AiFilterBind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(function);
}

void RegisterPromptSqlFunction(ExtensionLoader &loader, const std::string &name) {
	auto function =
	    ScalarFunction(name, {LogicalType::VARCHAR}, LogicalType::VARCHAR, AiPromptSqlFunction, AiPromptSqlBind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(function);
}

AggregateFunction BuildAiAggregateFunction(const std::string &name, idx_t argument_count,
                                           bind_aggregate_function_t bind) {
	vector<LogicalType> arguments;
	arguments.emplace_back(LogicalType::VARCHAR);
	for (idx_t i = 1; i < argument_count; i++) {
		arguments.emplace_back(LogicalType::ANY);
	}
	auto function = AggregateFunction(
	    name, std::move(arguments), LogicalType::VARCHAR, AggregateFunction::StateSize<AiAggregateState>,
	    AggregateFunction::StateInitialize<AiAggregateState, AiAggregateOperation>,
	    AggregateFunction::UnaryScatterUpdate<AiAggregateState, string_t, AiAggregateOperation>,
	    AggregateFunction::StateCombine<AiAggregateState, AiAggregateOperation>,
	    AggregateFunction::StateFinalize<AiAggregateState, string_t, AiAggregateOperation>,
	    FunctionNullHandling::DEFAULT_NULL_HANDLING,
	    AggregateFunction::UnaryUpdate<AiAggregateState, string_t, AiAggregateOperation>, bind);
	function.SetFallible();
	function.SetVolatile();
	return function;
}

void RegisterAiAggregateFunction(ExtensionLoader &loader, const std::string &name, bind_aggregate_function_t bind,
                                 idx_t min_argument_count, idx_t max_argument_count) {
	AggregateFunctionSet functions(name);
	for (idx_t argument_count = min_argument_count; argument_count <= max_argument_count; argument_count++) {
		functions.AddFunction(BuildAiAggregateFunction(name, argument_count, bind));
	}
	loader.RegisterFunction(functions);
}

void AddCompletionNamedParameters(TableFunction &function, bool include_response_options,
                                  bool include_log_payload_options) {
	static const char *completion_options[] = {"model",
	                                           "provider",
	                                           "profile",
	                                           "secret",
	                                           "secret_name",
	                                           "temperature",
	                                           "system_prompt",
	                                           "max_tokens",
	                                           "base_url",
	                                           "timeout_seconds",
	                                           "retry_count",
	                                           "retry_backoff_ms",
	                                           "max_concurrent_requests",
	                                           "min_request_interval_ms",
	                                           "input_token_price_per_million",
	                                           "output_token_price_per_million",
	                                           "use_builtin_model_prices",
	                                           "log_format",
	                                           "log_tags",
	                                           "log_sample_rate",
	                                           "fail_on_error"};
	for (auto name : completion_options) {
		function.named_parameters[name] = OptionType(name);
	}
	if (include_response_options) {
		function.named_parameters["response_format"] = LogicalType::VARCHAR;
		function.named_parameters["response_schema"] = LogicalType::VARCHAR;
		function.named_parameters["json_schema"] = LogicalType::VARCHAR;
	}
	if (include_log_payload_options) {
		function.named_parameters["log_include_text"] = LogicalType::BOOLEAN;
		function.named_parameters["log_strict"] = LogicalType::BOOLEAN;
	}
}

void AddPromptQueryNamedParameters(TableFunction &function) {
	function.named_parameters["schema_context"] = LogicalType::VARCHAR;
	function.named_parameters["schema"] = LogicalType::VARCHAR;
	function.named_parameters["include_tables"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["exclude_tables"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["sample_rows"] = LogicalType::BIGINT;
	AddCompletionNamedParameters(function, true, false);
}

void AddPromptSchemaNamedParameters(TableFunction &function) {
	function.named_parameters["include_tables"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["exclude_tables"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["sample_rows"] = LogicalType::BIGINT;
}

void AddAiRecordNamedParameters(TableFunction &function) {
	AddCompletionNamedParameters(function, false, true);
}

void AddPromptAssistantNamedParameters(TableFunction &function, bool include_error) {
	AddPromptQueryNamedParameters(function);
	if (include_error) {
		function.named_parameters["error"] = LogicalType::VARCHAR;
	}
}

void RegisterPromptSchemaFunction(ExtensionLoader &loader, const std::string &name) {
	TableFunctionSet ai_schema_prompt(name);
	vector<vector<LogicalType>> overloads = {
	    {},
	    {LogicalType::VARCHAR},
	    {LogicalType::LIST(LogicalType::VARCHAR)},
	};
	for (auto &arguments : overloads) {
		TableFunction function(std::move(arguments), PromptSchemaFunction, PromptSchemaBind, PromptSchemaInit);
		AddPromptSchemaNamedParameters(function);
		ai_schema_prompt.AddFunction(std::move(function));
	}
	loader.RegisterFunction(std::move(ai_schema_prompt));
}

void RegisterAiRecordFunction(ExtensionLoader &loader) {
	TableFunctionSet ai_complete_record("ai_complete_record");
	for (idx_t argument_count = 2; argument_count <= 4; argument_count++) {
		vector<LogicalType> arguments;
		for (idx_t i = 0; i < argument_count; i++) {
			arguments.emplace_back(LogicalType::VARCHAR);
		}
		TableFunction function(std::move(arguments), AiRecordFunction, AiRecordBind, AiRecordInit);
		AddAiRecordNamedParameters(function);
		ai_complete_record.AddFunction(std::move(function));
	}
	loader.RegisterFunction(std::move(ai_complete_record));
}

void RegisterPromptExplainFunction(ExtensionLoader &loader, const std::string &name) {
	TableFunctionSet ai_explain_sql(name);
	TableFunction function({LogicalType::VARCHAR}, PromptAssistantFunction, PromptExplainBind, PromptAssistantInit);
	AddPromptAssistantNamedParameters(function, false);
	ai_explain_sql.AddFunction(std::move(function));
	loader.RegisterFunction(std::move(ai_explain_sql));
}

void RegisterPromptFixupFunction(ExtensionLoader &loader, const std::string &name) {
	TableFunctionSet ai_fix_sql(name);
	TableFunction function({LogicalType::VARCHAR}, PromptAssistantFunction, PromptFixupBind, PromptAssistantInit);
	AddPromptAssistantNamedParameters(function, false);
	ai_fix_sql.AddFunction(std::move(function));
	loader.RegisterFunction(std::move(ai_fix_sql));
}

void RegisterPromptFixLineFunction(ExtensionLoader &loader, const std::string &name) {
	TableFunctionSet ai_fix_sql_line(name);
	vector<vector<LogicalType>> overloads = {
	    {LogicalType::VARCHAR},
	    {LogicalType::VARCHAR, LogicalType::VARCHAR},
	};
	for (auto &arguments : overloads) {
		TableFunction function(std::move(arguments), PromptAssistantFunction, PromptFixLineBind, PromptAssistantInit);
		AddPromptAssistantNamedParameters(function, true);
		ai_fix_sql_line.AddFunction(std::move(function));
	}
	loader.RegisterFunction(std::move(ai_fix_sql_line));
}

void RegisterPromptQueryFunction(ExtensionLoader &loader, const std::string &name) {
	TableFunctionSet ai_query_data(name);
	for (idx_t argument_count = 1; argument_count <= 4; argument_count++) {
		vector<LogicalType> arguments;
		for (idx_t i = 0; i < argument_count; i++) {
			arguments.emplace_back(LogicalType::VARCHAR);
		}
		TableFunction function(std::move(arguments), nullptr, nullptr);
		function.bind_replace = PromptQueryBindReplace;
		AddPromptQueryNamedParameters(function);
		ai_query_data.AddFunction(std::move(function));
	}
	loader.RegisterFunction(std::move(ai_query_data));
}

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	RegisterSettings(loader);
	RegisterAiProviderSecret(loader);

	RegisterCompletionFunction(loader, "ai_complete", AiCompleteBind);
	RegisterCompletionFunction(loader, "ai_request_json", AiRequestJsonBind);

	auto ai_complete_json = ScalarFunction("ai_complete_json", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                       AiCompletionFunction, AiCompleteJsonBind);
	ai_complete_json.varargs = LogicalType::ANY;
	ai_complete_json.SetFallible();
	ai_complete_json.SetVolatile();
	ai_complete_json.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(ai_complete_json);
	RegisterAiRecordFunction(loader);

	RegisterEmbeddingFunction(loader, "ai_embed");

	auto ai_embedding_request_json =
	    ScalarFunction("ai_embedding_request_json", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                   AiEmbeddingRequestJsonFunction, AiEmbeddingRequestJsonBind);
	ai_embedding_request_json.varargs = LogicalType::ANY;
	ai_embedding_request_json.SetFallible();
	ai_embedding_request_json.SetVolatile();
	ai_embedding_request_json.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(ai_embedding_request_json);

	auto ai_similarity = ScalarFunction("ai_similarity", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                    LogicalType::DOUBLE, AiSimilarityFunction, AiSimilarityBind);
	ai_similarity.varargs = LogicalType::ANY;
	ai_similarity.SetFallible();
	ai_similarity.SetVolatile();
	ai_similarity.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(ai_similarity);

	RegisterTaskFunction(loader, "ai_summarize", {LogicalType::VARCHAR}, AiSummarizeBind);
	RegisterTaskFunction(loader, "ai_sentiment", {LogicalType::VARCHAR}, AiSentimentBind);
	RegisterTaskFunction(loader, "ai_fix_grammar", {LogicalType::VARCHAR}, AiFixGrammarBind);
	RegisterTaskFunction(loader, "ai_redact", {LogicalType::VARCHAR}, AiMaskBind);
	RegisterTaskFunction(loader, "ai_translate", {LogicalType::VARCHAR, LogicalType::VARCHAR}, AiTranslateBind);
	RegisterTaskFunction(loader, "ai_classify", {LogicalType::VARCHAR, LogicalType::VARCHAR}, AiClassifyBind);
	RegisterTaskFunction(loader, "ai_extract", {LogicalType::VARCHAR, LogicalType::VARCHAR}, AiExtractBind);
	RegisterAiFilterFunction(loader);

	RegisterAiAggregateFunction(loader, "ai_agg", AiAggBind, 2, 10);
	RegisterAiAggregateFunction(loader, "ai_summarize_agg", AiSummarizeAggBind, 1, 9);

	RegisterPromptSqlFunction(loader, "ai_sql");

	RegisterPromptSchemaFunction(loader, "ai_schema_prompt");
	RegisterPromptExplainFunction(loader, "ai_explain_sql");
	RegisterPromptFixupFunction(loader, "ai_fix_sql");
	RegisterPromptFixLineFunction(loader, "ai_fix_sql_line");
	RegisterPromptQueryFunction(loader, "ai_query_data");

	loader.RegisterFunction(
	    ScalarFunction("ai_is_read_only_sql", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, AiIsReadOnlySqlFunction));
	loader.RegisterFunction(ScalarFunction("ai_validate_read_only_sql", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                       AiValidateReadOnlySqlFunction));
	loader.RegisterFunction(
	    ScalarFunction("ai_provider_base_url", {LogicalType::VARCHAR}, LogicalType::VARCHAR, AiProviderBaseUrl));
	loader.RegisterFunction(
	    ScalarFunction("ai_provider_protocol", {LogicalType::VARCHAR}, LogicalType::VARCHAR, AiProviderProtocol));

	auto ai_count_tokens = ScalarFunction("ai_count_tokens", {LogicalType::VARCHAR}, LogicalType::BIGINT,
	                                      AiCountTokensFunction, AiCountTokensBind);
	ai_count_tokens.varargs = LogicalType::ANY;
	ai_count_tokens.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(ai_count_tokens);

	loader.RegisterFunction(TableFunction("ai_usage", {}, AiUsageFunction, AiUsageBind, AiUsageInit));
	loader.RegisterFunction(
	    TableFunction("ai_clear_usage", {}, AiClearUsageFunction, AiClearUsageBind, AiClearUsageInit));
	loader.RegisterFunction(TableFunction("ai_secrets", {}, AiSecretsFunction, AiSecretsBind, AiSecretsInit));
	loader.RegisterFunction(
	    TableFunction("ai_models", {}, AiModelPricesFunction, AiModelPricesBind, AiModelPricesInit));
}

void DuckdbAiExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string DuckdbAiExtension::Name() {
	return "duckdb_ai";
}

std::string DuckdbAiExtension::Version() const {
#ifdef EXT_VERSION_DUCKDB_AI
	return EXT_VERSION_DUCKDB_AI;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(duckdb_ai, loader) {
	duckdb::LoadInternal(loader);
}
}
