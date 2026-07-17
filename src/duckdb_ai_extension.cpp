#define DUCKDB_EXTENSION_MAIN

#include "ai_extension.hpp"
#include "duckdb_ai_provider.hpp"
#include "yyjson.hpp"

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
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace duckdb {

namespace {

constexpr int64_t MAX_TOKEN_LIMIT_PER_MINUTE = 10000000000LL;
constexpr idx_t MAX_PROVIDER_CHUNK_WORKERS = 64;
constexpr size_t MAX_PROMPT_QUERY_CACHE_ENTRIES = 1024;
constexpr size_t MAX_SIMILARITY_QUERY_CACHES = 8;
constexpr size_t MAX_SIMILARITY_QUERY_CACHE_BYTES = 8 * 1024 * 1024;
constexpr int64_t MAX_SQL_FIX_ATTEMPTS = 5;
std::atomic<uint64_t> NEXT_QUERY_ID {1};

// Tripwire so CompletionOptionsEqual is updated when CompletionOptions gains fields. The size is
// ABI-specific (std::string layout differs per standard library), so only enforce it on the
// toolchains where the expected values are known instead of breaking every other build.
#if defined(__APPLE__)
static_assert(sizeof(duckdb_ai::CompletionOptions) == 688 || sizeof(duckdb_ai::CompletionOptions) == 808,
              "Update CompletionOptionsEqual when CompletionOptions fields change");
#endif

bool CompletionOptionsEqual(const duckdb_ai::CompletionOptions &left, const duckdb_ai::CompletionOptions &right) {
	// Request operation IDs intentionally do not affect batching or provider configuration equality.
	return left.model == right.model && left.provider == right.provider && left.secret_name == right.secret_name &&
	       left.explicit_model == right.explicit_model && left.explicit_provider == right.explicit_provider &&
	       left.explicit_base_url == right.explicit_base_url && left.system_prompt == right.system_prompt &&
	       left.base_url == right.base_url && left.api_key == right.api_key &&
	       left.has_temperature == right.has_temperature && left.temperature == right.temperature &&
	       left.has_max_tokens == right.has_max_tokens && left.max_tokens == right.max_tokens &&
	       left.has_timeout_seconds == right.has_timeout_seconds && left.timeout_seconds == right.timeout_seconds &&
	       left.has_retry_count == right.has_retry_count && left.retry_count == right.retry_count &&
	       left.has_retry_backoff_ms == right.has_retry_backoff_ms && left.retry_backoff_ms == right.retry_backoff_ms &&
	       left.has_max_concurrent_requests == right.has_max_concurrent_requests &&
	       left.max_concurrent_requests == right.max_concurrent_requests &&
	       left.has_min_request_interval_ms == right.has_min_request_interval_ms &&
	       left.min_request_interval_ms == right.min_request_interval_ms &&
	       left.has_token_limit_per_minute == right.has_token_limit_per_minute &&
	       left.token_limit_per_minute == right.token_limit_per_minute &&
	       left.has_input_token_price_per_million == right.has_input_token_price_per_million &&
	       left.input_token_price_per_million == right.input_token_price_per_million &&
	       left.has_output_token_price_per_million == right.has_output_token_price_per_million &&
	       left.output_token_price_per_million == right.output_token_price_per_million &&
	       left.has_use_builtin_model_prices == right.has_use_builtin_model_prices &&
	       left.use_builtin_model_prices == right.use_builtin_model_prices && left.has_cache == right.has_cache &&
	       left.cache == right.cache && left.has_cache_ttl_seconds == right.has_cache_ttl_seconds &&
	       left.cache_ttl_seconds == right.cache_ttl_seconds &&
	       left.has_response_cache_max_entries == right.has_response_cache_max_entries &&
	       left.response_cache_max_entries == right.response_cache_max_entries &&
	       left.has_prompt_cache == right.has_prompt_cache && left.prompt_cache == right.prompt_cache &&
	       left.has_connect_timeout_seconds == right.has_connect_timeout_seconds &&
	       left.connect_timeout_seconds == right.connect_timeout_seconds && left.allowed_hosts == right.allowed_hosts &&
	       left.log_endpoint == right.log_endpoint && left.log_format == right.log_format &&
	       left.log_tags == right.log_tags && left.has_log_include_text == right.has_log_include_text &&
	       left.log_include_text == right.log_include_text && left.has_log_strict == right.has_log_strict &&
	       left.log_strict == right.log_strict && left.has_log_sample_rate == right.has_log_sample_rate &&
	       left.log_sample_rate == right.log_sample_rate && left.fail_on_error == right.fail_on_error &&
	       left.on_error == right.on_error && left.response_format == right.response_format &&
	       left.response_schema == right.response_schema && left.function_name == right.function_name &&
	       left.query_id == right.query_id && left.model_options == right.model_options;
}

void AddTableColumns(vector<LogicalType> &return_types, vector<string> &names,
                     std::initializer_list<std::pair<const char *, LogicalType>> columns) {
	for (auto &column : columns) {
		names.emplace_back(column.first);
		return_types.emplace_back(column.second);
	}
}

void StampProviderFunction(duckdb_ai::CompletionOptions &options, const std::string &function_name) {
	options.function_name = function_name;
	if (options.query_id.empty()) {
		options.query_id = "duckdb_ai_query_" + std::to_string(NEXT_QUERY_ID.fetch_add(1));
	}
}

struct AiUsageScanData : public GlobalTableFunctionState {
	explicit AiUsageScanData(vector<duckdb_ai::UsageEvent> events_p) : offset(0), events(std::move(events_p)) {
	}

	idx_t offset;
	vector<duckdb_ai::UsageEvent> events;
};

struct AiUsageSummaryRow {
	std::string query_id;
	std::string provider;
	std::string model;
	uint64_t calls = 0;
	uint64_t retries = 0;
	uint64_t failures = 0;
	uint64_t cache_hits = 0;
	std::set<std::string> operation_ids;
	int64_t total_tokens = 0;
	int64_t elapsed_ms = 0;
	double estimated_cost_usd = 0;
};

struct AiUsageSummaryScanData : public GlobalTableFunctionState {
	idx_t offset = 0;
	vector<AiUsageSummaryRow> rows;
	duckdb_ai::UsageBufferStats stats;
};

struct AiTextChunk {
	std::string source_id;
	std::string chunk_id;
	idx_t chunk_index;
	idx_t start_offset;
	idx_t end_offset;
	std::string text;
	std::string heading;
	idx_t page;
};

struct AiChunkBindData : public FunctionData {
	std::string input;
	std::string source_id;
	std::string title;
	std::string metadata;
	std::string strategy = "recursive";
	idx_t chunk_size = 1000;
	double overlap_percent = 10;
	bool prep_search = false;
	bool enrich = false;
	duckdb_ai::CompletionOptions options;
	vector<AiTextChunk> chunks;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<AiChunkBindData>();
		result->input = input;
		result->source_id = source_id;
		result->title = title;
		result->metadata = metadata;
		result->strategy = strategy;
		result->chunk_size = chunk_size;
		result->overlap_percent = overlap_percent;
		result->prep_search = prep_search;
		result->enrich = enrich;
		result->options = options;
		result->chunks = chunks;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AiChunkBindData>();
		return input == other.input && source_id == other.source_id && title == other.title &&
		       metadata == other.metadata && strategy == other.strategy && chunk_size == other.chunk_size &&
		       overlap_percent == other.overlap_percent && prep_search == other.prep_search && enrich == other.enrich &&
		       CompletionOptionsEqual(options, other.options);
	}
};

struct AiChunkScanData : public GlobalTableFunctionState {
	idx_t offset = 0;
	bool prepared = false;
	std::string document_context;
	std::string enrichment_error;
};

struct ExternalModelBindData : public FunctionData {
	std::string name;
	std::string provider;
	std::string model;
	std::string location;
	std::string credential;
	std::string model_type;
	std::string capabilities;
	std::string options;
	bool replace = false;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ExternalModelBindData>();
		result->name = name;
		result->provider = provider;
		result->model = model;
		result->location = location;
		result->credential = credential;
		result->model_type = model_type;
		result->capabilities = capabilities;
		result->options = options;
		result->replace = replace;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ExternalModelBindData>();
		return name == other.name && provider == other.provider && model == other.model && location == other.location &&
		       credential == other.credential && model_type == other.model_type && capabilities == other.capabilities &&
		       options == other.options && replace == other.replace;
	}
};

enum class ControlPlaneOperation : uint8_t { PROVISION, STATUS, DEPROVISION };

struct ControlPlaneBindData : public FunctionData {
	explicit ControlPlaneBindData(ControlPlaneOperation operation_p) : operation(operation_p) {
	}

	ControlPlaneOperation operation;
	std::string argument;
	bool dry_run = true;
	bool has_max_hourly_cost = false;
	double max_hourly_cost_usd = 0;
	duckdb_ai::CompletionOptions options;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ControlPlaneBindData>(operation);
		result->argument = argument;
		result->dry_run = dry_run;
		result->has_max_hourly_cost = has_max_hourly_cost;
		result->max_hourly_cost_usd = max_hourly_cost_usd;
		result->options = options;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ControlPlaneBindData>();
		return operation == other.operation && argument == other.argument && dry_run == other.dry_run &&
		       has_max_hourly_cost == other.has_max_hourly_cost && max_hourly_cost_usd == other.max_hourly_cost_usd &&
		       CompletionOptionsEqual(options, other.options);
	}
};

struct DocumentParseBindData : public FunctionData {
	std::string content;
	std::string mime_type;
	std::string parser_profile;
	std::string pages;
	duckdb_ai::CompletionOptions options;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<DocumentParseBindData>();
		result->content = content;
		result->mime_type = mime_type;
		result->parser_profile = parser_profile;
		result->pages = pages;
		result->options = options;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<DocumentParseBindData>();
		return content == other.content && mime_type == other.mime_type && parser_profile == other.parser_profile &&
		       pages == other.pages && CompletionOptionsEqual(options, other.options);
	}
};

struct DocumentElement {
	std::string document_id;
	int64_t page = -1;
	int64_t element_index = -1;
	std::string element_type;
	std::string text;
	std::string markdown;
	std::string bbox;
	double confidence = 0;
	bool has_confidence = false;
	std::string metadata;
	std::string error;
};

struct DocumentParseScanData : public GlobalTableFunctionState {
	idx_t offset = 0;
	bool loaded = false;
	vector<DocumentElement> elements;
};

struct ClassifierBuildBindData : public FunctionData {
	std::vector<std::string> labels;
	duckdb_ai::CompletionOptions label_options;
	duckdb_ai::CompletionOptions embedding_options;
	idx_t sample_size = 256;
	double quality_threshold = 0.8;
	double confidence_margin = 0.05;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ClassifierBuildBindData>();
		result->labels = labels;
		result->label_options = label_options;
		result->embedding_options = embedding_options;
		result->sample_size = sample_size;
		result->quality_threshold = quality_threshold;
		result->confidence_margin = confidence_margin;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ClassifierBuildBindData>();
		return labels == other.labels && CompletionOptionsEqual(label_options, other.label_options) &&
		       CompletionOptionsEqual(embedding_options, other.embedding_options) && sample_size == other.sample_size &&
		       quality_threshold == other.quality_threshold && confidence_margin == other.confidence_margin;
	}

	bool SupportStatementCache() const override {
		return false;
	}
};

struct ClassifierBuildState {
	idx_t size;
	idx_t alloc_size;
	idx_t sample_count;
	idx_t row_count;
	char *dataptr;
};

struct OptimizedClassifierBindData : public FunctionData {
	duckdb_ai::CompletionOptions fallback_options;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<OptimizedClassifierBindData>();
		result->fallback_options = fallback_options;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		return CompletionOptionsEqual(fallback_options, other_p.Cast<OptimizedClassifierBindData>().fallback_options);
	}
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

struct AiModelsScanData : public GlobalTableFunctionState {
	idx_t offset = 0;
	vector<SecretEntry> models;
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

struct PromptQueryCacheState : public ObjectCacheEntry {
	static std::string ObjectType() {
		return "duckdb_ai_prompt_query_cache";
	}

	std::string GetObjectType() override {
		return ObjectType();
	}

	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}

	std::mutex mutex;
	std::unordered_map<std::string, std::string> generated_sql;
	std::deque<std::string> recency_order;
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
	int64_t fix_attempts = 0;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<PromptAssistantBindData>(kind);
		result->options = options;
		result->sql = sql;
		result->error_message = error_message;
		result->schema_context = schema_context;
		result->fix_attempts = fix_attempts;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<PromptAssistantBindData>();
		return kind == other.kind && CompletionOptionsEqual(options, other.options) && sql == other.sql &&
		       error_message == other.error_message && schema_context == other.schema_context &&
		       fix_attempts == other.fix_attempts;
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
int64_t ReadFixAttemptsValue(const Value &value_p, const std::string &function_name);

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

struct AiExtractRecordBindData : public FunctionData {
	std::string response_schema;
	duckdb_ai::CompletionOptions options;
	vector<AiRecordColumn> columns;
	bool has_model_arg = false;
	idx_t model_index = 0;
	bool has_provider_arg = false;
	idx_t provider_index = 0;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<AiExtractRecordBindData>();
		result->response_schema = response_schema;
		result->options = options;
		result->columns = columns;
		result->has_model_arg = has_model_arg;
		result->model_index = model_index;
		result->has_provider_arg = has_provider_arg;
		result->provider_index = provider_index;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AiExtractRecordBindData>();
		return response_schema == other.response_schema && CompletionOptionsEqual(options, other.options) &&
		       AiRecordColumnsEqual(columns, other.columns) && has_model_arg == other.has_model_arg &&
		       model_index == other.model_index && has_provider_arg == other.has_provider_arg &&
		       provider_index == other.provider_index;
	}
};

enum class AiTaskKind : uint8_t { SUMMARIZE, SENTIMENT, FIX_GRAMMAR, REDACT, TRANSLATE, CLASSIFY, EXTRACT, FILTER };

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
	bool parameter_is_label_list = false;
	std::string label_descriptions;
	std::string instructions;
	std::string examples;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<AiTaskBindData>(task, required_args);
		result->options = options;
		result->has_model_arg = has_model_arg;
		result->model_index = model_index;
		result->has_provider_arg = has_provider_arg;
		result->provider_index = provider_index;
		result->parameter_is_label_list = parameter_is_label_list;
		result->label_descriptions = label_descriptions;
		result->instructions = instructions;
		result->examples = examples;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AiTaskBindData>();
		return task == other.task && required_args == other.required_args &&
		       CompletionOptionsEqual(options, other.options) && has_model_arg == other.has_model_arg &&
		       model_index == other.model_index && has_provider_arg == other.has_provider_arg &&
		       provider_index == other.provider_index && parameter_is_label_list == other.parameter_is_label_list &&
		       label_descriptions == other.label_descriptions && instructions == other.instructions &&
		       examples == other.examples;
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
	std::string overflow_policy = "hierarchical";

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<AiAggregateBindData>(kind);
		result->options = options;
		result->instruction = instruction;
		result->separator = separator;
		result->max_context_chars = max_context_chars;
		result->overflow_policy = overflow_policy;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AiAggregateBindData>();
		return kind == other.kind && CompletionOptionsEqual(options, other.options) &&
		       instruction == other.instruction && separator == other.separator &&
		       max_context_chars == other.max_context_chars && overflow_policy == other.overflow_policy;
	}

	bool SupportStatementCache() const override {
		return false;
	}
};

struct AiAggregateState {
	idx_t size;
	idx_t alloc_size;
	idx_t row_count;
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
	int64_t fix_attempts = 0;

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
		result->fix_attempts = fix_attempts;
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
		       provider_index == other.provider_index && fix_attempts == other.fix_attempts;
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
	    name == "json_schema" || name == "allowed_hosts" || name == "log_format" || name == "log_tags" ||
	    name == "on_error") {
		type = LogicalType::VARCHAR;
		return true;
	}
	if (name == "temperature" || name == "log_sample_rate" || name == "input_token_price_per_million" ||
	    name == "output_token_price_per_million") {
		type = LogicalType::DOUBLE;
		return true;
	}
	if (name == "max_tokens" || name == "retry_count" || name == "retry_backoff_ms" ||
	    name == "max_concurrent_requests" || name == "min_request_interval_ms" || name == "token_limit_per_minute" ||
	    name == "connect_timeout_seconds" || name == "cache_max_entries") {
		type = LogicalType::BIGINT;
		return true;
	}
	if (name == "timeout_seconds" || name == "cache_ttl_seconds") {
		type = LogicalType::BIGINT;
		return true;
	}
	if (name == "cache" || name == "prompt_cache" || name == "fail_on_error" || name == "use_builtin_model_prices") {
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
	                      "max_concurrent_requests, min_request_interval_ms, token_limit_per_minute, cache, "
	                      "cache_ttl_seconds, cache_max_entries, prompt_cache, connect_timeout_seconds, "
	                      "allowed_hosts, "
	                      "input_token_price_per_million, output_token_price_per_million, use_builtin_model_prices, "
	                      "fail_on_error, on_error, profile, secret, response_format, response_schema, json_schema, "
	                      "log_format, log_tags, log_sample_rate",
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
	throw BinderException("%s option \"log_format\" must be one of: generic, json, generic_json, otlp, otlp_json",
	                      function_name);
}

std::string NormalizeOnErrorValue(const std::string &value, const std::string &function_name) {
	auto on_error = LowerAscii(value);
	if (on_error == "fail" || on_error == "null" || on_error == "capture") {
		return on_error;
	}
	throw BinderException("%s option \"on_error\" must be one of: fail, null, capture", function_name);
}

bool ApplyCompletionValueOption(duckdb_ai::CompletionOptions &options, const std::string &function_name,
                                const std::string &name, const Value &value, bool allow_response_options,
                                bool allow_log_payload_options) {
	if (name == "model") {
		options.model = OptionStringValue(value, function_name, name);
		options.explicit_model = true;
		return true;
	}
	if (name == "provider") {
		options.provider = OptionStringValue(value, function_name, name);
		options.explicit_provider = true;
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
		options.explicit_base_url = true;
		return true;
	}
	if (name == "allowed_hosts") {
		options.allowed_hosts = OptionStringValue(value, function_name, name);
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
		if (!std::isfinite(options.temperature) || options.temperature < 0 || options.temperature > 2) {
			throw BinderException("%s option \"temperature\" must be between 0 and 2", function_name);
		}
		options.has_temperature = true;
		return true;
	}
	if (name == "log_sample_rate") {
		options.log_sample_rate = OptionDoubleValue(value, function_name, name);
		if (!std::isfinite(options.log_sample_rate) || options.log_sample_rate < 0 || options.log_sample_rate > 1) {
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
	if (name == "cache") {
		options.cache = OptionBoolValue(value, function_name, name);
		options.has_cache = true;
		return true;
	}
	if (name == "cache_ttl_seconds") {
		options.cache_ttl_seconds = OptionBigIntValue(value, function_name, name);
		if (options.cache_ttl_seconds < 0 || options.cache_ttl_seconds > 31536000) {
			throw BinderException("%s option \"cache_ttl_seconds\" must be between 0 and 31536000", function_name);
		}
		options.has_cache_ttl_seconds = true;
		return true;
	}
	if (name == "cache_max_entries") {
		options.response_cache_max_entries = OptionBigIntValue(value, function_name, name);
		if (options.response_cache_max_entries < 0 || options.response_cache_max_entries > 1000000) {
			throw BinderException("%s option \"cache_max_entries\" must be between 0 and 1000000", function_name);
		}
		options.has_response_cache_max_entries = true;
		return true;
	}
	if (name == "prompt_cache") {
		options.prompt_cache = OptionBoolValue(value, function_name, name);
		options.has_prompt_cache = true;
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
		if (options.max_concurrent_requests < 0 ||
		    options.max_concurrent_requests > static_cast<int64_t>(MAX_PROVIDER_CHUNK_WORKERS)) {
			throw BinderException("%s option \"max_concurrent_requests\" must be between 0 and %llu", function_name,
			                      static_cast<unsigned long long>(MAX_PROVIDER_CHUNK_WORKERS));
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
	if (name == "token_limit_per_minute") {
		options.token_limit_per_minute = OptionBigIntValue(value, function_name, name);
		if (options.token_limit_per_minute < 0 || options.token_limit_per_minute > MAX_TOKEN_LIMIT_PER_MINUTE) {
			throw BinderException("%s option \"token_limit_per_minute\" must be between 0 and %lld", function_name,
			                      static_cast<long long>(MAX_TOKEN_LIMIT_PER_MINUTE));
		}
		options.has_token_limit_per_minute = true;
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
	if (name == "connect_timeout_seconds") {
		options.connect_timeout_seconds = OptionBigIntValue(value, function_name, name);
		if (options.connect_timeout_seconds <= 0 || options.connect_timeout_seconds > 31536000) {
			throw BinderException("%s option \"connect_timeout_seconds\" must be between 1 and 31536000",
			                      function_name);
		}
		options.has_connect_timeout_seconds = true;
		return true;
	}
	if (name == "fail_on_error") {
		options.fail_on_error = OptionBoolValue(value, function_name, name);
		options.on_error = options.fail_on_error ? "fail" : "null";
		return true;
	}
	if (name == "on_error") {
		options.on_error = NormalizeOnErrorValue(OptionStringValue(value, function_name, name), function_name);
		options.fail_on_error = options.on_error == "fail";
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
	if (name == "overflow_policy") {
		auto option = EvaluateConstantOption(context, expr, name, LogicalType::VARCHAR);
		auto value = LowerAscii(StringValue::Get(option));
		if (value != "hierarchical" && value != "error") {
			throw BinderException("AI aggregate option \"overflow_policy\" must be hierarchical or error");
		}
		bind_data.overflow_policy = value;
		return;
	}
	ApplyNamedOption(context, bind_data.options, expr, name);
}

bool TryGetSetting(ClientContext &context, const std::string &name, Value &value) {
	return context.TryGetCurrentSetting(name, value) && !value.IsNull();
}

enum class AiModelSettingKind { GENERIC, COMPLETION, TASK, AGGREGATE, EMBEDDING, SQL_ASSISTANT };

std::string ModelSettingName(AiModelSettingKind kind) {
	switch (kind) {
	case AiModelSettingKind::COMPLETION:
		return "duckdb_ai_completion_model";
	case AiModelSettingKind::TASK:
		return "duckdb_ai_task_model";
	case AiModelSettingKind::AGGREGATE:
		return "duckdb_ai_aggregate_model";
	case AiModelSettingKind::EMBEDDING:
		return "duckdb_ai_embedding_model";
	case AiModelSettingKind::SQL_ASSISTANT:
		return "duckdb_ai_sql_assistant_model";
	case AiModelSettingKind::GENERIC:
		break;
	}
	return "";
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
	if (!std::isfinite(setting)) {
		throw BinderException("Setting %s must be between 0 and 1", name);
	}
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
	ApplyStringSetting(context, prefix + "_allowed_hosts", options.allowed_hosts);
	ApplyStringSetting(context, prefix + "_response_format", options.response_format);
	ApplyStringSetting(context, prefix + "_response_schema", options.response_schema);
	ApplyStringSetting(context, prefix + "_log_endpoint", options.log_endpoint);
	ApplyStringSetting(context, prefix + "_log_format", options.log_format);
	ApplyStringSetting(context, prefix + "_log_tags", options.log_tags);
	ApplyStringSetting(context, prefix + "_on_error", options.on_error);
	ApplyBoolSetting(context, prefix + "_log_include_text", options.log_include_text, options.has_log_include_text);
	ApplyBoolSetting(context, prefix + "_log_strict", options.log_strict, options.has_log_strict);
	ApplyDoubleSetting(context, prefix + "_log_sample_rate", options.log_sample_rate, options.has_log_sample_rate);
	ApplyBigIntRangeSetting(context, prefix + "_retry_count", options.retry_count, options.has_retry_count, 0, 10);
	ApplyBigIntRangeSetting(context, prefix + "_retry_backoff_ms", options.retry_backoff_ms,
	                        options.has_retry_backoff_ms, 0, 60000);
	ApplyOptionalBigIntRangeSetting(context, prefix + "_max_concurrent_requests", options.max_concurrent_requests,
	                                options.has_max_concurrent_requests, 0,
	                                static_cast<int64_t>(MAX_PROVIDER_CHUNK_WORKERS));
	ApplyOptionalBigIntRangeSetting(context, prefix + "_min_request_interval_ms", options.min_request_interval_ms,
	                                options.has_min_request_interval_ms, 0, 60000);
	ApplyOptionalBigIntRangeSetting(context, prefix + "_token_limit_per_minute", options.token_limit_per_minute,
	                                options.has_token_limit_per_minute, 0, MAX_TOKEN_LIMIT_PER_MINUTE);
	ApplyNonNegativeDoubleSetting(context, prefix + "_input_token_price_per_million",
	                              options.input_token_price_per_million, options.has_input_token_price_per_million);
	ApplyNonNegativeDoubleSetting(context, prefix + "_output_token_price_per_million",
	                              options.output_token_price_per_million, options.has_output_token_price_per_million);
	ApplyBoolSetting(context, prefix + "_use_builtin_model_prices", options.use_builtin_model_prices,
	                 options.has_use_builtin_model_prices);
	ApplyBoolSetting(context, prefix + "_cache", options.cache, options.has_cache);
	ApplyOptionalBigIntRangeSetting(context, prefix + "_cache_ttl_seconds", options.cache_ttl_seconds,
	                                options.has_cache_ttl_seconds, 0, 31536000);
	ApplyOptionalBigIntRangeSetting(context, prefix + "_cache_max_entries", options.response_cache_max_entries,
	                                options.has_response_cache_max_entries, 0, 1000000);
	ApplyBoolSetting(context, prefix + "_prompt_cache", options.prompt_cache, options.has_prompt_cache);
	ApplyOptionalBigIntRangeSetting(context, prefix + "_connect_timeout_seconds", options.connect_timeout_seconds,
	                                options.has_connect_timeout_seconds, 1, 31536000);
	ApplyTimeoutSetting(context, prefix + "_timeout_seconds", options);
}

void ApplyModelSetting(ClientContext &context, AiModelSettingKind kind, duckdb_ai::CompletionOptions &options) {
	auto setting_name = ModelSettingName(kind);
	if (!setting_name.empty()) {
		ApplyStringSetting(context, setting_name, options.model);
	}
}

void ApplySettings(ClientContext &context, duckdb_ai::CompletionOptions &options,
                   AiModelSettingKind kind = AiModelSettingKind::GENERIC) {
	ApplySettingsWithPrefix(context, "duckdb_ai", options);
	ApplyModelSetting(context, kind, options);
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
			throw BinderException(
			    "Setting duckdb_ai_log_format must be one of: generic, json, generic_json, otlp, otlp_json");
		}
	}
	if (!options.on_error.empty()) {
		options.on_error = NormalizeOnErrorValue(options.on_error, "AI setting");
		options.fail_on_error = options.on_error == "fail";
	}
	duckdb_ai::SnapshotEnvironmentOptions(options);
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

std::string ExternalModelSecretName(const std::string &name) {
	return "__duckdb_ai_model_" + name;
}

unique_ptr<BaseSecret> CreateAiModelSecretFromConfig(ClientContext &, CreateSecretInput &input) {
	auto provider = OptionalSecretInputString(input, "ai_provider");
	auto model = OptionalSecretInputString(input, "model");
	if (provider.empty() || model.empty()) {
		throw InvalidInputException("duckdb_ai_model entries require AI_PROVIDER and MODEL");
	}
	provider = duckdb_ai::NormalizeProviderName(provider);
	auto scope = input.scope;
	if (scope.empty()) {
		scope.push_back(provider);
	}
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	secret->secret_map["provider"] = Value(provider);
	secret->secret_map["model"] = Value(model);
	for (auto key : {"location", "credential", "model_type", "capabilities", "options"}) {
		auto value = OptionalSecretInputString(input, key);
		if (!value.empty()) {
			secret->secret_map[key] = Value(value);
		}
	}
	return std::move(secret);
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
		auto profile_name = options.secret_name;
		auto model_entry = secret_manager.GetSecretByName(transaction, ExternalModelSecretName(profile_name));
		if (model_entry && model_entry->secret && model_entry->secret->GetType() == "duckdb_ai_model") {
			const auto &model_secret = dynamic_cast<const KeyValueSecret &>(*model_entry->secret);
			std::string value;
			if (!options.explicit_provider && TryReadSecretString(model_secret, "provider", value)) {
				options.provider = value;
			}
			value.clear();
			if (!options.explicit_model && TryReadSecretString(model_secret, "model", value)) {
				options.model = value;
			}
			value.clear();
			if (!options.explicit_base_url && TryReadSecretString(model_secret, "location", value)) {
				options.base_url = value;
			}
			value.clear();
			if (TryReadSecretString(model_secret, "options", value)) {
				options.model_options = value;
				duckdb_ai::ApplyModelProfileOptions(options);
			}
			std::string credential;
			if (!TryReadSecretString(model_secret, "credential", credential)) {
				options.secret_name.clear();
				return;
			}
			options.secret_name = credential;
		}
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
	ApplySettings(context, bind_data->options, AiModelSettingKind::COMPLETION);
	StampProviderFunction(bind_data->options, bound_function.name);
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

unique_ptr<FunctionData> AiCompletionRequestJsonBind(ClientContext &context, ScalarFunction &bound_function,
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

std::string QuoteClassificationLabel(const std::string &label) {
	std::string result = "\"";
	for (auto c : label) {
		if (c == '"' || c == '\\') {
			result += '\\';
		}
		result += c;
	}
	result += "\"";
	return result;
}

std::string FormatClassificationLabels(const vector<std::string> &labels) {
	std::string result;
	for (idx_t i = 0; i < labels.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += QuoteClassificationLabel(labels[i]);
	}
	return result;
}

struct StringListVectorReader {
	StringListVectorReader(DataChunk &args, idx_t column) {
		auto &list_vector = args.data[column];
		list_vector.ToUnifiedFormat(args.size(), data);
		entries = UnifiedVectorFormat::GetData<list_entry_t>(data);
		auto &child_vector = ListVector::GetEntry(list_vector);
		child_vector.ToUnifiedFormat(ListVector::GetListSize(list_vector), child_data);
		child_values = UnifiedVectorFormat::GetData<string_t>(child_data);
	}

	bool Read(idx_t row, std::string &value) const {
		auto mapped_row = data.sel->get_index(row);
		if (!data.validity.RowIsValid(mapped_row)) {
			return false;
		}
		auto entry = entries[mapped_row];
		if (entry.length == 0) {
			return false;
		}
		vector<std::string> labels;
		labels.reserve(entry.length);
		for (idx_t i = 0; i < entry.length; i++) {
			auto child_row = child_data.sel->get_index(entry.offset + i);
			if (!child_data.validity.RowIsValid(child_row)) {
				return false;
			}
			labels.push_back(child_values[child_row].GetString());
		}
		value = FormatClassificationLabels(labels);
		return true;
	}

	UnifiedVectorFormat data;
	UnifiedVectorFormat child_data;
	const list_entry_t *entries;
	const string_t *child_values;
};

struct ProviderJobBase {
	idx_t row = 0;
	duckdb_ai::CompletionOptions options;
	std::exception_ptr exception;
	std::string error_message;
	bool fail_on_error = true;
	bool completed = false;
};

struct ProviderStringJob : public ProviderJobBase {
	std::string input;
	std::string parameter;
	std::string output;
};

struct ProviderStringListJob : public ProviderJobBase {
	std::string input;
	std::string parameter;
	std::vector<std::string> output;
};

struct ProviderBoolJob : public ProviderJobBase {
	std::string input;
	std::string parameter;
	bool output = false;
};

struct ProviderEmbeddingJob : public ProviderJobBase {
	std::string input;
	std::vector<double> output;
};

struct ProviderDoubleJob : public ProviderJobBase {
	std::string left;
	std::string right;
	bool has_left_embedding = false;
	bool has_right_embedding = false;
	std::vector<double> left_embedding;
	std::vector<double> right_embedding;
	double output = 0;
};

struct SecretResolutionCacheEntry {
	duckdb_ai::CompletionOptions input_options;
	duckdb_ai::CompletionOptions resolved_options;
};

struct ProviderExecutorState : public ObjectCacheEntry {
	~ProviderExecutorState() override {
		{
			std::lock_guard<std::mutex> lock(mutex);
			shutdown = true;
		}
		cv.notify_all();
		for (auto &worker : workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
	}

	static std::string ObjectType() {
		return "duckdb_ai_provider_executor";
	}

	std::string GetObjectType() override {
		return ObjectType();
	}

	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}

	void EnsureWorkers(idx_t requested_workers) {
		std::lock_guard<std::mutex> lock(mutex);
		while (workers.size() < requested_workers) {
			workers.emplace_back([this]() {
				while (true) {
					std::function<void()> task;
					{
						std::unique_lock<std::mutex> lock {mutex};
						cv.wait(lock, [&]() { return shutdown || !this->queue.empty(); });
						if (shutdown && this->queue.empty()) {
							return;
						}
						task = std::move(this->queue.front());
						this->queue.pop_front();
					}
					task();
				}
			});
		}
	}

	void Submit(std::function<void()> task) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			queue.push_back(std::move(task));
		}
		cv.notify_one();
	}

	std::mutex mutex;
	std::condition_variable cv;
	std::deque<std::function<void()>> queue;
	std::vector<std::thread> workers;
	bool shutdown = false;
};

struct SimilarityOptionCache {
	duckdb_ai::CompletionOptions options;
	size_t credential_fingerprint = 0;
	std::unordered_map<std::string, std::vector<double>> embeddings;
};

bool SimilarityCacheOptionsEqual(const SimilarityOptionCache &cached, const duckdb_ai::CompletionOptions &options) {
	if (cached.credential_fingerprint != std::hash<std::string> {}(options.api_key)) {
		return false;
	}
	auto sanitized = options;
	sanitized.api_key.clear();
	return CompletionOptionsEqual(cached.options, sanitized);
}

struct SimilarityQueryCache {
	std::mutex mutex;
	std::vector<SimilarityOptionCache> option_groups;
	size_t estimated_bytes = 0;
};

struct SimilarityCacheState : public ObjectCacheEntry {
	static std::string ObjectType() {
		return "duckdb_ai_similarity_query_cache";
	}

	std::string GetObjectType() override {
		return ObjectType();
	}

	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}

	std::mutex mutex;
	std::unordered_map<idx_t, std::shared_ptr<SimilarityQueryCache>> queries;
	std::deque<idx_t> recency_order;
};

ProviderExecutorState fallback_provider_executor;

ProviderExecutorState &ProviderExecutor(const duckdb_ai::CompletionOptions &options) {
	if (!options.client_context) {
		return fallback_provider_executor;
	}
	return *ObjectCache::GetObjectCache(*options.client_context)
	            .GetOrCreate<ProviderExecutorState>(ProviderExecutorState::ObjectType());
}

std::shared_ptr<SimilarityQueryCache> SimilarityCache(ClientContext &context) {
	auto &state =
	    *ObjectCache::GetObjectCache(context).GetOrCreate<SimilarityCacheState>(SimilarityCacheState::ObjectType());
	auto query_id = context.transaction.HasActiveTransaction() ? context.transaction.GetActiveQuery() : 0;
	std::lock_guard<std::mutex> lock(state.mutex);
	auto entry = state.queries.find(query_id);
	if (entry != state.queries.end()) {
		return entry->second;
	}
	while (state.queries.size() >= MAX_SIMILARITY_QUERY_CACHES && !state.recency_order.empty()) {
		state.queries.erase(state.recency_order.front());
		state.recency_order.pop_front();
	}
	auto cache = std::make_shared<SimilarityQueryCache>();
	state.queries.emplace(query_id, cache);
	state.recency_order.push_back(query_id);
	return cache;
}

void ClearSimilarityCache(ClientContext &context) {
	auto &state =
	    *ObjectCache::GetObjectCache(context).GetOrCreate<SimilarityCacheState>(SimilarityCacheState::ObjectType());
	std::lock_guard<std::mutex> lock(state.mutex);
	state.queries.clear();
	state.recency_order.clear();
}

void ApplyAiProviderSecretCached(ClientContext &context, duckdb_ai::CompletionOptions &options,
                                 std::vector<SecretResolutionCacheEntry> &cache) {
	for (auto &entry : cache) {
		if (CompletionOptionsEqual(entry.input_options, options)) {
			options = entry.resolved_options;
			return;
		}
	}
	SecretResolutionCacheEntry entry;
	entry.input_options = options;
	ApplyAiProviderSecret(context, options);
	entry.resolved_options = options;
	cache.push_back(std::move(entry));
}

template <class JOB>
idx_t ProviderWorkerCount(const std::vector<JOB> &jobs) {
	if (jobs.size() <= 1) {
		return 1;
	}
	idx_t configured_workers = 0;
	for (auto &job : jobs) {
		auto cap = duckdb_ai::EffectiveMaxConcurrentRequests(job.options);
		if (cap > 0) {
			auto bounded_cap = static_cast<idx_t>(std::min<int64_t>(cap, MAX_PROVIDER_CHUNK_WORKERS));
			configured_workers =
			    configured_workers == 0 ? bounded_cap : MinValue<idx_t>(configured_workers, bounded_cap);
		}
	}
	idx_t worker_count = configured_workers;
	if (worker_count == 0) {
		auto hardware_workers = std::thread::hardware_concurrency();
		worker_count = hardware_workers == 0 ? 4 : static_cast<idx_t>(hardware_workers);
		worker_count = std::min<idx_t>(worker_count, 8);
	}
	worker_count = std::min<idx_t>(worker_count, jobs.size());
	return std::max<idx_t>(1, std::min<idx_t>(worker_count, MAX_PROVIDER_CHUNK_WORKERS));
}

template <class JOB, class CALLBACK>
void RunProviderJobs(std::vector<JOB> &jobs, CALLBACK callback) {
	if (jobs.empty()) {
		return;
	}
	auto worker_count = ProviderWorkerCount(jobs);
	std::atomic<bool> stop {false};
	auto run_one = [&](JOB &job) {
		try {
			callback(job);
			job.completed = true;
		} catch (std::exception &ex) {
			job.exception = std::current_exception();
			job.error_message = ex.what();
			if (job.fail_on_error) {
				stop.store(true);
			}
		} catch (...) {
			job.exception = std::current_exception();
			job.error_message = "unknown AI provider error";
			if (job.fail_on_error) {
				stop.store(true);
			}
		}
	};

	if (worker_count <= 1) {
		for (auto &job : jobs) {
			if (stop.load()) {
				return;
			}
			run_one(job);
		}
		return;
	}

	struct BatchWaitState {
		std::mutex mutex;
		std::condition_variable cv;
		idx_t remaining = 0;
	};
	auto wait_state = std::make_shared<BatchWaitState>();
	wait_state->remaining = worker_count;
	auto next_job = std::make_shared<std::atomic<idx_t>>(0);
	auto &executor = ProviderExecutor(jobs[0].options);
	executor.EnsureWorkers(worker_count);
	for (idx_t worker = 0; worker < worker_count; worker++) {
		executor.Submit([&, next_job, wait_state]() {
			while (!stop.load()) {
				auto job_index = next_job->fetch_add(1);
				if (job_index >= jobs.size()) {
					break;
				}
				run_one(jobs[job_index]);
			}
			{
				std::lock_guard<std::mutex> lock(wait_state->mutex);
				wait_state->remaining--;
			}
			wait_state->cv.notify_one();
		});
	}
	std::unique_lock<std::mutex> lock(wait_state->mutex);
	wait_state->cv.wait(lock, [&]() { return wait_state->remaining == 0; });
}

bool ReadRuntimeOptions(ClientContext &context, const duckdb_ai::CompletionOptions &base_options, bool has_model_arg,
                        const StringVectorReader *model_reader, bool has_provider_arg,
                        const StringVectorReader *provider_reader, idx_t row, duckdb_ai::CompletionOptions &options) {
	options = base_options;
	duckdb_ai::AttachProviderRuntimeState(options, context);
	if (has_model_arg) {
		if (!model_reader->Read(row, options.model)) {
			return false;
		}
		options.explicit_model = true;
	}
	if (has_provider_arg) {
		if (!provider_reader->Read(row, options.provider)) {
			return false;
		}
		options.explicit_provider = true;
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

std::string BuildJsonCompletionSystemPrompt(const std::string &response_schema) {
	std::string output =
	    "Return only valid JSON. Do not include markdown, code fences, comments, or explanatory text.\n";
	if (!response_schema.empty()) {
		output += "The JSON response must follow this JSON Schema:\n";
		output += response_schema;
		output += "\n";
	} else {
		output += "The JSON response must be a top-level object or array.\n";
	}
	return output;
}

void ApplyJsonCompletionSystemPrompt(duckdb_ai::CompletionOptions &options, const std::string &response_schema) {
	auto json_system_prompt = BuildJsonCompletionSystemPrompt(response_schema);
	if (options.system_prompt.empty()) {
		options.system_prompt = std::move(json_system_prompt);
		return;
	}
	options.system_prompt += "\n\n";
	options.system_prompt += json_system_prompt;
}

void AppendSystemPrompt(duckdb_ai::CompletionOptions &options, const std::string &system_prompt) {
	if (system_prompt.empty()) {
		return;
	}
	if (options.system_prompt.empty()) {
		options.system_prompt = system_prompt;
		return;
	}
	options.system_prompt += "\n\n";
	options.system_prompt += system_prompt;
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

	std::vector<ProviderStringJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string prompt;
		if (!prompt_reader.Read(row, prompt)) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(state.GetContext(), bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderStringJob job;
			job.row = row;
			if (bind_data.validate_json_output) {
				ApplyJsonCompletionSystemPrompt(options, options.response_schema);
			}
			job.options = options;
			job.fail_on_error = options.fail_on_error;
			job.input = std::move(prompt);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}

	RunProviderJobs(jobs, [&](ProviderStringJob &job) {
		job.output = bind_data.request_json ? duckdb_ai::BuildRequestJson(job.input, job.options)
		                                    : duckdb_ai::Complete(job.input, job.options).text;
		if (bind_data.validate_json_output) {
			std::string error;
			if (!ValidateJsonCompletionOutput(job.output, job.options.response_schema, error)) {
				throw InvalidInputException("ai_complete_json expected valid JSON output: %s", error);
			}
		}
	});

	for (auto &job : jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		result_data[job.row] = StringVector::AddString(result, job.output);
	}
}

LogicalType AiTryCompleteReturnType() {
	child_list_t<LogicalType> children;
	children.emplace_back("response", LogicalType::VARCHAR);
	children.emplace_back("error", LogicalType::VARCHAR);
	return LogicalType::STRUCT(std::move(children));
}

unique_ptr<FunctionData> AiTryCompleteBind(ClientContext &context, ScalarFunction &bound_function,
                                           vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = AiCompletionBindInternal(context, bound_function, arguments);
	auto &completion_bind_data = bind_data->Cast<AiCompletionBindData>();
	completion_bind_data.options.on_error = "capture";
	completion_bind_data.options.fail_on_error = false;
	bound_function.SetReturnType(AiTryCompleteReturnType());
	return bind_data;
}

Value AiTryCompleteValue(const LogicalType &result_type, const std::string &response, bool has_error,
                         const std::string &error) {
	vector<Value> children;
	children.reserve(2);
	if (has_error) {
		children.emplace_back(LogicalType::VARCHAR);
		children.emplace_back(error);
	} else {
		children.emplace_back(response);
		children.emplace_back(LogicalType::VARCHAR);
	}
	return Value::STRUCT(result_type, std::move(children));
}

void AiTryCompleteFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiCompletionBindData>();
	auto result_type = AiTryCompleteReturnType();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader prompt_reader(args, bind_data.prompt_index);
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	std::vector<ProviderStringJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string prompt;
		if (!prompt_reader.Read(row, prompt)) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(state.GetContext(), bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderStringJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = false;
			job.input = prompt;
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			result.SetValue(row, AiTryCompleteValue(result_type, "", true, ex.what()));
		}
	}

	RunProviderJobs(jobs,
	                [&](ProviderStringJob &job) { job.output = duckdb_ai::Complete(job.input, job.options).text; });

	for (auto &job : jobs) {
		if (job.exception) {
			result.SetValue(job.row, AiTryCompleteValue(result_type, "", true, job.error_message));
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		result.SetValue(job.row, AiTryCompleteValue(result_type, job.output, false, ""));
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
	ApplySettings(context, bind_data->options, AiModelSettingKind::COMPLETION);
	StampProviderFunction(bind_data->options, "ai_complete_record");
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
		duckdb_ai::AttachProviderRuntimeState(options, context);
		ApplyAiProviderSecret(context, options);
		ApplyJsonCompletionSystemPrompt(options, bind_data.response_schema);
		auto result = duckdb_ai::Complete(bind_data.prompt, options).text;
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

Value AiRecordStructValue(const vector<AiRecordColumn> &columns, const vector<duckdb_ai::JsonExtractedValue> &values) {
	vector<Value> children;
	children.reserve(columns.size());
	for (idx_t column_index = 0; column_index < columns.size(); column_index++) {
		children.push_back(AiRecordValue(values[column_index], columns[column_index]));
	}
	return Value::STRUCT(AiRecordStructType(columns), std::move(children));
}

unique_ptr<FunctionData> AiExtractRecordBind(ClientContext &context, ScalarFunction &bound_function,
                                             vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() < 2) {
		throw BinderException("%s requires text and response_schema arguments", bound_function.name);
	}
	auto bind_data = make_uniq<AiExtractRecordBindData>();
	ApplySettings(context, bind_data->options, AiModelSettingKind::COMPLETION);
	StampProviderFunction(bind_data->options, bound_function.name);
	bind_data->response_schema =
	    StringValue::Get(EvaluateConstantOption(context, *arguments[1], "response_schema", LogicalType::VARCHAR));
	ValidateResponseSchemaValue(bind_data->response_schema, bound_function.name);
	bind_data->options.response_schema = bind_data->response_schema;
	bind_data->options.response_format = "json_schema";

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
				throw BinderException("%s supports at most two positional options after response_schema: model and "
				                      "provider",
				                      bound_function.name);
			}
			continue;
		}
		if (alias == "response_schema" || alias == "json_schema" || alias == "response_format") {
			throw BinderException("%s takes response_schema as the second argument", bound_function.name);
		}
		ApplyNamedOption(context, bind_data->options, *arguments[i], alias);
		bound_function.arguments.emplace_back(OptionType(alias));
	}

	vector<duckdb_ai::JsonSchemaProperty> properties;
	std::string error;
	if (!duckdb_ai::ExtractJsonSchemaProperties(bind_data->response_schema, properties, error)) {
		throw BinderException("%s response_schema cannot be projected as a record: %s", bound_function.name, error);
	}
	for (auto &property : properties) {
		bind_data->columns.push_back(BuildAiRecordColumn(property));
	}
	bound_function.SetReturnType(AiRecordStructType(bind_data->columns));
	return std::move(bind_data);
}

void AiExtractRecordFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiExtractRecordBindData>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader input_reader(args, 0);
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	std::vector<ProviderStringJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string input_text;
		if (!input_reader.Read(row, input_text)) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(state.GetContext(), bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ApplyJsonCompletionSystemPrompt(options, bind_data.response_schema);
			ProviderStringJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = options.fail_on_error;
			job.input = std::move(input_text);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}

	RunProviderJobs(jobs,
	                [&](ProviderStringJob &job) { job.output = duckdb_ai::Complete(job.input, job.options).text; });

	for (auto &job : jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		std::string error;
		if (!ValidateJsonCompletionOutput(job.output, job.options.response_schema, error)) {
			if (job.fail_on_error) {
				throw InvalidInputException("ai_extract_record expected valid JSON output: %s", error);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		vector<std::string> field_names;
		field_names.reserve(bind_data.columns.size());
		for (auto &column : bind_data.columns) {
			field_names.push_back(column.name);
		}
		vector<duckdb_ai::JsonExtractedValue> values;
		if (!duckdb_ai::ExtractJsonObjectFields(job.output, field_names, values, error)) {
			if (job.fail_on_error) {
				throw InvalidInputException("ai_extract_record could not project JSON output: %s", error);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		result.SetValue(job.row, AiRecordStructValue(bind_data.columns, values));
	}
}

bool IsEmbeddingOption(const std::string &name) {
	return name == "model" || name == "provider" || name == "profile" || name == "secret" || name == "secret_name" ||
	       name == "base_url" || name == "timeout_seconds" || name == "retry_count" || name == "retry_backoff_ms" ||
	       name == "max_concurrent_requests" || name == "min_request_interval_ms" || name == "token_limit_per_minute" ||
	       name == "cache_ttl_seconds" || name == "cache_max_entries" || name == "connect_timeout_seconds" ||
	       name == "input_token_price_per_million" || name == "output_token_price_per_million" ||
	       name == "use_builtin_model_prices" || name == "cache" || name == "allowed_hosts" ||
	       name == "fail_on_error" || name == "on_error" || name == "log_format" || name == "log_tags" ||
	       name == "log_sample_rate";
}

unique_ptr<FunctionData> AiEmbeddingBindInternal(ClientContext &context, ScalarFunction &bound_function,
                                                 vector<unique_ptr<Expression>> &arguments, bool request_json) {
	auto bind_data = make_uniq<AiCompletionBindData>(request_json);
	ApplySettings(context, bind_data->options, AiModelSettingKind::EMBEDDING);
	StampProviderFunction(bind_data->options, bound_function.name);
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
			    "on_error, max_concurrent_requests, min_request_interval_ms, token_limit_per_minute, "
			    "cache, cache_ttl_seconds, cache_max_entries, allowed_hosts, connect_timeout_seconds, "
			    "input_token_price_per_million, output_token_price_per_million, use_builtin_model_prices, "
			    "log_format, log_tags, log_sample_rate",
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
	ApplySettings(context, bind_data->options, AiModelSettingKind::EMBEDDING);
	StampProviderFunction(bind_data->options, bound_function.name);
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
			    "on_error, max_concurrent_requests, min_request_interval_ms, token_limit_per_minute, cache, "
			    "cache_ttl_seconds, cache_max_entries, allowed_hosts, connect_timeout_seconds, "
			    "input_token_price_per_million, output_token_price_per_million, use_builtin_model_prices, "
			    "log_format, log_tags, log_sample_rate",
			    alias);
		}
		ApplyNamedOption(context, bind_data->options, *arguments[i], alias);
		bound_function.arguments.emplace_back(OptionType(alias));
	}
	bound_function.SetReturnType(LogicalType::DOUBLE);
	return std::move(bind_data);
}

unique_ptr<FunctionData> AiRerankBind(ClientContext &context, ScalarFunction &bound_function,
                                      vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<AiCompletionBindData>(false);
	ApplySettings(context, bind_data->options, AiModelSettingKind::COMPLETION);
	StampProviderFunction(bind_data->options, bound_function.name);
	if (arguments.size() < 2) {
		throw BinderException("%s requires query and candidate text arguments", bound_function.name);
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
		ApplyNamedOption(context, bind_data->options, *arguments[i], alias);
		bound_function.arguments.emplace_back(OptionType(alias));
	}
	bound_function.SetReturnType(LogicalType::DOUBLE);
	return std::move(bind_data);
}

void ReadEmbeddingInputs(ClientContext &context, const StringVectorReader &input_reader,
                         const StringVectorReader *model_reader, const StringVectorReader *provider_reader,
                         const AiCompletionBindData &bind_data, idx_t row, std::string &input,
                         duckdb_ai::CompletionOptions &options, bool &is_null) {
	if (!input_reader.Read(row, input)) {
		is_null = true;
		return;
	}
	if (!ReadRuntimeOptions(context, bind_data.options, bind_data.has_model_arg, model_reader,
	                        bind_data.has_provider_arg, provider_reader, row, options)) {
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

bool IsAsciiWhitespace(char c);
std::string StripMarkdownJsonFence(const std::string &text);

std::string BuildRerankSystemPrompt() {
	return "Score candidate relevance to the search query on a scale from 0 to 1. Return only one decimal number. "
	       "Use 0 for not relevant and 1 for exactly relevant.";
}

std::string BuildRerankPrompt(const std::string &query, const std::string &candidate) {
	return "Query:\n" + query + "\n\nCandidate:\n" + candidate;
}

std::string BuildScoreSystemPrompt() {
	return "Score how well the input satisfies the supplied criteria on a scale from 0 to 1. Return only a JSON "
	       "object with one numeric field named score. Use 0 when it does not satisfy the criteria and 1 when it fully "
	       "satisfies them.";
}

std::string BuildScorePrompt(const std::string &input, const std::string &criteria) {
	return "Criteria:\n" + criteria + "\n\nInput:\n" + input;
}

bool ParseRerankScore(const std::string &input, double &score) {
	auto trimmed = StripMarkdownJsonFence(input);
	if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
		trimmed = trimmed.substr(1, trimmed.size() - 2);
	}
	auto *begin = trimmed.c_str();
	char *end = nullptr;
	errno = 0;
	auto value = std::strtod(begin, &end);
	if (begin == end || errno == ERANGE) {
		return false;
	}
	while (end && *end && IsAsciiWhitespace(*end)) {
		end++;
	}
	if (end && *end) {
		return false;
	}
	if (!std::isfinite(value) || value < 0 || value > 1) {
		return false;
	}
	score = value;
	return true;
}

bool ParseStructuredScore(const std::string &input, double &score) {
	std::vector<duckdb_ai::JsonExtractedValue> fields;
	std::string error;
	if (!duckdb_ai::ExtractJsonObjectFields(StripMarkdownJsonFence(input), {"score"}, fields, error) ||
	    fields.size() != 1 || fields[0].kind != duckdb_ai::JsonExtractedKind::NUMBER ||
	    !std::isfinite(fields[0].number_value) || fields[0].number_value < 0 || fields[0].number_value > 1) {
		return false;
	}
	score = fields[0].number_value;
	return true;
}

void AiRerankFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiCompletionBindData>();
	auto generic_score = func_expr.function.name == "ai_score";

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader query_reader(args, 0);
	StringVectorReader candidate_reader(args, 1);
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	std::vector<ProviderDoubleJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string query;
		std::string candidate;
		if (!query_reader.Read(row, query) || !candidate_reader.Read(row, candidate)) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(state.GetContext(), bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderDoubleJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = options.fail_on_error;
			job.left = std::move(query);
			job.right = std::move(candidate);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}

	RunProviderJobs(jobs, [&](ProviderDoubleJob &job) {
		if (generic_score) {
			job.options.response_format = "json_schema";
			job.options.response_schema =
			    R"({"type":"object","properties":{"score":{"type":"number","minimum":0,"maximum":1}},"required":["score"],"additionalProperties":false})";
		}
		AppendSystemPrompt(job.options, generic_score ? BuildScoreSystemPrompt() : BuildRerankSystemPrompt());
		auto prompt = generic_score ? BuildScorePrompt(job.left, job.right) : BuildRerankPrompt(job.left, job.right);
		auto output = duckdb_ai::Complete(prompt, job.options).text;
		auto parsed = generic_score ? ParseStructuredScore(output, job.output) : ParseRerankScore(output, job.output);
		if (!parsed) {
			throw InvalidInputException("%s expected a numeric score from 0 to 1, got: %s", func_expr.function.name,
			                            output);
		}
	});

	for (auto &job : jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		result_data[job.row] = job.output;
	}
}

void PrecomputeSimilarityEmbeddings(ClientContext &context, std::vector<ProviderDoubleJob> &jobs) {
	auto query_cache = SimilarityCache(context);
	std::vector<bool> grouped(jobs.size(), false);
	for (idx_t group_start = 0; group_start < jobs.size(); group_start++) {
		if (grouped[group_start] || jobs[group_start].exception) {
			continue;
		}
		std::vector<idx_t> group_rows;
		for (idx_t row = group_start; row < jobs.size(); row++) {
			if (!grouped[row] && !jobs[row].exception &&
			    CompletionOptionsEqual(jobs[group_start].options, jobs[row].options)) {
				grouped[row] = true;
				group_rows.push_back(row);
			}
		}

		std::vector<std::string> unique_inputs;
		std::unordered_map<std::string, idx_t> input_indexes;
		for (auto row : group_rows) {
			for (auto *input : {&jobs[row].left, &jobs[row].right}) {
				if (input_indexes.find(*input) == input_indexes.end()) {
					input_indexes[*input] = unique_inputs.size();
					unique_inputs.push_back(*input);
				}
			}
		}

		try {
			std::unique_lock<std::mutex> cache_lock(query_cache->mutex);
			SimilarityOptionCache *option_cache = nullptr;
			for (auto &candidate : query_cache->option_groups) {
				if (SimilarityCacheOptionsEqual(candidate, jobs[group_start].options)) {
					option_cache = &candidate;
					break;
				}
			}
			if (!option_cache) {
				SimilarityOptionCache candidate;
				candidate.options = jobs[group_start].options;
				candidate.credential_fingerprint = std::hash<std::string> {}(candidate.options.api_key);
				candidate.options.api_key.clear();
				query_cache->option_groups.push_back(std::move(candidate));
				option_cache = &query_cache->option_groups.back();
			}

			std::vector<const std::vector<double> *> resolved_embeddings(unique_inputs.size(), nullptr);
			std::vector<std::string> missing_inputs;
			std::vector<idx_t> missing_indexes;
			for (idx_t input_index = 0; input_index < unique_inputs.size(); input_index++) {
				auto cached = option_cache->embeddings.find(unique_inputs[input_index]);
				if (cached != option_cache->embeddings.end()) {
					resolved_embeddings[input_index] = &cached->second;
					duckdb_ai::RecordEmbeddingCacheHit(unique_inputs[input_index],
					                                   NumericCast<int64_t>(cached->second.size()),
					                                   jobs[group_start].options);
				} else {
					missing_indexes.push_back(input_index);
					missing_inputs.push_back(unique_inputs[input_index]);
				}
			}

			auto embeddings = duckdb_ai::EmbedMany(missing_inputs, jobs[group_start].options);
			if (embeddings.size() != missing_inputs.size()) {
				throw InvalidInputException("AI embedding provider returned %llu embeddings for %llu inputs",
				                            static_cast<unsigned long long>(embeddings.size()),
				                            static_cast<unsigned long long>(missing_inputs.size()));
			}
			for (idx_t missing_index = 0; missing_index < missing_inputs.size(); missing_index++) {
				auto input_index = missing_indexes[missing_index];
				auto estimated_bytes = missing_inputs[missing_index].size() +
				                       embeddings[missing_index].values.size() * sizeof(double) + 64;
				if (query_cache->estimated_bytes + estimated_bytes <= MAX_SIMILARITY_QUERY_CACHE_BYTES) {
					auto inserted = option_cache->embeddings.emplace(missing_inputs[missing_index],
					                                                 std::move(embeddings[missing_index].values));
					resolved_embeddings[input_index] = &inserted.first->second;
					query_cache->estimated_bytes += estimated_bytes;
				} else {
					resolved_embeddings[input_index] = &embeddings[missing_index].values;
				}
			}
			for (auto row : group_rows) {
				jobs[row].left_embedding = *resolved_embeddings[input_indexes[jobs[row].left]];
				jobs[row].right_embedding = *resolved_embeddings[input_indexes[jobs[row].right]];
				jobs[row].has_left_embedding = true;
				jobs[row].has_right_embedding = true;
			}
		} catch (std::exception &ex) {
			for (auto row : group_rows) {
				jobs[row].exception = std::current_exception();
				jobs[row].error_message = ex.what();
			}
			if (jobs[group_start].fail_on_error) {
				throw;
			}
		}
	}
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

	std::vector<ProviderDoubleJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string left;
		std::string right;
		if (!left_reader.Read(row, left) || !right_reader.Read(row, right)) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(state.GetContext(), bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderDoubleJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = options.fail_on_error;
			job.left = std::move(left);
			job.right = std::move(right);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}

	PrecomputeSimilarityEmbeddings(state.GetContext(), jobs);

	RunProviderJobs(jobs, [&](ProviderDoubleJob &job) {
		if (job.exception) {
			return;
		}
		job.output = CosineSimilarity(job.left_embedding, job.right_embedding);
	});

	for (auto &job : jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		result_data[job.row] = job.output;
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

	std::vector<ProviderStringJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		duckdb_ai::CompletionOptions options;
		bool is_null = false;
		ReadEmbeddingInputs(state.GetContext(), input_reader, model_reader.get(), provider_reader.get(), bind_data, row,
		                    input, options, is_null);
		if (is_null) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderStringJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = options.fail_on_error;
			job.input = std::move(input);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}

	RunProviderJobs(jobs, [&](ProviderStringJob &job) {
		job.output = duckdb_ai::BuildEmbeddingRequestJson(job.input, job.options);
	});

	for (auto &job : jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		result_data[job.row] = StringVector::AddString(result, job.output);
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

	std::vector<ProviderEmbeddingJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		duckdb_ai::CompletionOptions options;
		bool is_null = false;
		ReadEmbeddingInputs(state.GetContext(), input_reader, model_reader.get(), provider_reader.get(), bind_data, row,
		                    input, options, is_null);
		if (is_null) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderEmbeddingJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = options.fail_on_error;
			job.input = std::move(input);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}

	auto can_batch = jobs.size() > 1;
	for (idx_t i = 1; i < jobs.size() && can_batch; i++) {
		can_batch = CompletionOptionsEqual(jobs[0].options, jobs[i].options);
	}
	if (can_batch) {
		try {
			std::vector<std::string> inputs;
			inputs.reserve(jobs.size());
			for (auto &job : jobs) {
				inputs.push_back(job.input);
			}
			auto outputs = duckdb_ai::EmbedMany(inputs, jobs[0].options);
			if (outputs.size() != jobs.size()) {
				throw InvalidInputException("AI embedding provider returned %llu embeddings for %llu inputs",
				                            static_cast<unsigned long long>(outputs.size()),
				                            static_cast<unsigned long long>(jobs.size()));
			}
			for (idx_t i = 0; i < jobs.size(); i++) {
				jobs[i].output = std::move(outputs[i].values);
				jobs[i].completed = true;
			}
		} catch (std::exception &ex) {
			for (auto &job : jobs) {
				job.exception = std::current_exception();
				job.error_message = ex.what();
			}
		}
	} else {
		RunProviderJobs(
		    jobs, [&](ProviderEmbeddingJob &job) { job.output = duckdb_ai::Embed(job.input, job.options).values; });
	}

	for (auto &job : jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		result_data[job.row].offset = ListVector::GetListSize(result);
		result_data[job.row].length = job.output.size();
		for (auto value : job.output) {
			ListVector::PushBack(result, Value::DOUBLE(value));
		}
	}
}

unique_ptr<FunctionData> AiTaskBindInternal(ClientContext &context, ScalarFunction &bound_function,
                                            vector<unique_ptr<Expression>> &arguments, AiTaskKind task,
                                            idx_t required_args) {
	auto bind_data = make_uniq<AiTaskBindData>(task, required_args);
	ApplySettings(context, bind_data->options, AiModelSettingKind::TASK);
	StampProviderFunction(bind_data->options, bound_function.name);
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
		if (task == AiTaskKind::CLASSIFY &&
		    (alias == "label_descriptions" || alias == "instructions" || alias == "examples")) {
			auto value = EvaluateConstantOption(context, *arguments[i], alias, LogicalType::VARCHAR);
			auto text = StringValue::Get(value);
			if (alias == "label_descriptions") {
				bind_data->label_descriptions = std::move(text);
			} else if (alias == "instructions") {
				bind_data->instructions = std::move(text);
			} else {
				bind_data->examples = std::move(text);
			}
			bound_function.arguments.emplace_back(LogicalType::VARCHAR);
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

unique_ptr<FunctionData> AiRedactBind(ClientContext &context, ScalarFunction &bound_function,
                                      vector<unique_ptr<Expression>> &arguments) {
	return AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::REDACT, 1);
}

unique_ptr<FunctionData> AiTranslateBind(ClientContext &context, ScalarFunction &bound_function,
                                         vector<unique_ptr<Expression>> &arguments) {
	return AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::TRANSLATE, 2);
}

unique_ptr<FunctionData> AiClassifyBind(ClientContext &context, ScalarFunction &bound_function,
                                        vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::CLASSIFY, 2);
	if (arguments.size() >= 2 && arguments[1]->return_type.id() == LogicalTypeId::LIST) {
		auto &task_bind_data = bind_data->Cast<AiTaskBindData>();
		task_bind_data.parameter_is_label_list = true;
		bound_function.arguments[1] = LogicalType::LIST(LogicalType::VARCHAR);
	} else {
		bound_function.arguments[1] = LogicalType::VARCHAR;
	}
	return bind_data;
}

unique_ptr<FunctionData> AiClassifyLabelsBind(ClientContext &context, ScalarFunction &bound_function,
                                              vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = AiTaskBindInternal(context, bound_function, arguments, AiTaskKind::CLASSIFY, 2);
	if (arguments.size() >= 2 && arguments[1]->return_type.id() == LogicalTypeId::LIST) {
		auto &task_bind_data = bind_data->Cast<AiTaskBindData>();
		task_bind_data.parameter_is_label_list = true;
		bound_function.arguments[1] = LogicalType::LIST(LogicalType::VARCHAR);
	} else {
		bound_function.arguments[1] = LogicalType::VARCHAR;
	}
	bound_function.SetReturnType(LogicalType::LIST(LogicalType::VARCHAR));
	return bind_data;
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
	ApplySettings(context, bind_data->options, AiModelSettingKind::SQL_ASSISTANT);
	StampProviderFunction(bind_data->options, bound_function.name);
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
		if (alias == "fix_attempts") {
			auto value = EvaluateConstantOption(context, *arguments[i], alias, LogicalType::BIGINT);
			bind_data->fix_attempts = ReadFixAttemptsValue(value, bound_function.name);
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
	ApplySettings(context, bind_data->options, AiModelSettingKind::AGGREGATE);
	StampProviderFunction(bind_data->options, function.name);
	duckdb_ai::AttachProviderRuntimeState(bind_data->options, context);
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
	if (state.size > 0 && !bind_data.separator.empty()) {
		AiAggregateAppendBytes(state, allocator, bind_data.separator.c_str(), bind_data.separator.size());
	}
	AiAggregateAppendBytes(state, allocator, data, size);
}

std::string BuildAiAggregatePrompt(const AiAggregateBindData &bind_data, const std::string &values, idx_t row_count,
                                   bool intermediate) {
	std::string prompt;
	if (intermediate && bind_data.kind == AiAggregateKind::SUMMARIZE) {
		prompt = "Create a compact intermediate summary of this portion of a larger SQL aggregate. Preserve facts, "
		         "numbers, qualifications, and exceptions needed by the final summary. Return only the intermediate "
		         "summary.\n\nInput values:\n";
	} else if (intermediate) {
		prompt = "Extract and compact all evidence from this portion of a larger SQL aggregate that is relevant to the "
		         "instruction. Preserve facts, numbers, qualifications, and exceptions. Do not answer beyond the "
		         "provided evidence. Return only the compacted evidence.\n\nInstruction:\n" +
		         bind_data.instruction + "\n\nInput values:\n";
	} else if (bind_data.kind == AiAggregateKind::SUMMARIZE) {
		prompt =
		    "Summarize the following SQL aggregate input values concisely. Return only the summary.\n\nInput values:\n";
	} else {
		prompt =
		    "Use the following SQL aggregate input values to answer the instruction. Return only the final answer.\n\n"
		    "Instruction:\n" +
		    bind_data.instruction + "\n\nInput values:\n";
	}
	prompt += values;
	prompt += "\n\nInput row count: " + std::to_string(row_count);
	return prompt;
}

idx_t Utf8SafeChunkEnd(const std::string &input, idx_t start, idx_t requested_end) {
	auto end = MinValue<idx_t>(requested_end, input.size());
	while (end > start && end < input.size() && (static_cast<unsigned char>(input[end]) & 0xc0) == 0x80) {
		end--;
	}
	return end == start ? MinValue<idx_t>(requested_end, input.size()) : end;
}

std::vector<std::string> SplitAggregateValues(const std::string &values, idx_t max_chars) {
	std::vector<std::string> chunks;
	for (idx_t start = 0; start < values.size();) {
		auto end = Utf8SafeChunkEnd(values, start, start + max_chars);
		chunks.push_back(values.substr(start, end - start));
		start = end;
	}
	return chunks;
}

std::vector<std::string> PackAggregateValues(const std::vector<std::string> &values, const std::string &separator,
                                             idx_t max_chars) {
	std::vector<std::string> chunks;
	std::string packed;
	for (auto &value : values) {
		if (value.size() > max_chars) {
			if (!packed.empty()) {
				chunks.push_back(std::move(packed));
				packed.clear();
			}
			auto split = SplitAggregateValues(value, max_chars);
			chunks.insert(chunks.end(), std::make_move_iterator(split.begin()), std::make_move_iterator(split.end()));
			continue;
		}
		if (!packed.empty() && packed.size() + separator.size() + value.size() > max_chars) {
			chunks.push_back(std::move(packed));
			packed.clear();
		}
		if (!packed.empty()) {
			packed += separator;
		}
		packed += value;
	}
	if (!packed.empty()) {
		chunks.push_back(std::move(packed));
	}
	return chunks;
}

std::string RunHierarchicalAggregate(const AiAggregateBindData &bind_data, const std::string &values, idx_t row_count) {
	auto root_operation_id = bind_data.options.query_id + ":aggregate";
	if (values.size() <= bind_data.max_context_chars) {
		auto options = bind_data.options;
		options.parent_operation_id = root_operation_id;
		options.operation_id = root_operation_id + ":final";
		return duckdb_ai::Complete(BuildAiAggregatePrompt(bind_data, values, row_count, false), options).text;
	}
	if (bind_data.overflow_policy == "error") {
		throw InvalidInputException("AI aggregate input has %llu bytes, exceeding max_context_chars=%llu",
		                            static_cast<unsigned long long>(values.size()),
		                            static_cast<unsigned long long>(bind_data.max_context_chars));
	}

	auto chunks = SplitAggregateValues(values, bind_data.max_context_chars);
	for (idx_t level = 0; chunks.size() > 1; level++) {
		if (level >= 32) {
			throw InvalidInputException("AI aggregate hierarchical reduction did not converge after 32 levels");
		}
		auto previous_chunk_count = chunks.size();
		auto previous_bytes =
		    std::accumulate(chunks.begin(), chunks.end(), idx_t(0),
		                    [](idx_t total, const std::string &chunk) { return total + chunk.size(); });
		std::vector<ProviderStringJob> jobs;
		jobs.reserve(chunks.size());
		for (idx_t i = 0; i < chunks.size(); i++) {
			ProviderStringJob job;
			job.row = i;
			job.input = std::move(chunks[i]);
			job.options = bind_data.options;
			job.options.function_name += "_map";
			job.options.parent_operation_id = root_operation_id;
			job.options.operation_id = root_operation_id + ":map:" + std::to_string(level) + ":" + std::to_string(i);
			job.fail_on_error = bind_data.options.fail_on_error;
			jobs.push_back(std::move(job));
		}
		RunProviderJobs(jobs, [&](ProviderStringJob &job) {
			job.output =
			    duckdb_ai::Complete(BuildAiAggregatePrompt(bind_data, job.input, row_count, true), job.options).text;
		});
		std::vector<std::string> partials;
		partials.reserve(jobs.size());
		for (auto &job : jobs) {
			if (job.exception) {
				std::rethrow_exception(job.exception);
			}
			if (job.output.empty()) {
				throw InvalidInputException("AI aggregate provider returned an empty intermediate result");
			}
			partials.push_back(std::move(job.output));
		}
		auto next_chunks = PackAggregateValues(partials, bind_data.separator, bind_data.max_context_chars);
		if (next_chunks.empty()) {
			throw InvalidInputException("AI aggregate hierarchical reduction produced no intermediate results");
		}
		auto next_bytes = std::accumulate(next_chunks.begin(), next_chunks.end(), idx_t(0),
		                                  [](idx_t total, const std::string &chunk) { return total + chunk.size(); });
		if (next_chunks.size() >= previous_chunk_count && next_bytes >= previous_bytes) {
			throw InvalidInputException("AI aggregate hierarchical reduction made no progress at level %llu",
			                            static_cast<unsigned long long>(level));
		}
		chunks = std::move(next_chunks);
	}
	auto options = bind_data.options;
	options.parent_operation_id = root_operation_id;
	options.operation_id = root_operation_id + ":final";
	return duckdb_ai::Complete(BuildAiAggregatePrompt(bind_data, chunks[0], row_count, false), options).text;
}

struct AiAggregateOperation {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.size = 0;
		state.alloc_size = 0;
		state.row_count = 0;
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
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.row_count == 0) {
			finalize_data.ReturnNull();
			return;
		}
		auto &bind_data = finalize_data.input.bind_data->Cast<AiAggregateBindData>();
		try {
			std::string values;
			if (state.dataptr && state.size > 0) {
				values.assign(state.dataptr, state.size);
			}
			auto output = RunHierarchicalAggregate(bind_data, values, state.row_count);
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

std::string BuildTaskSystemPrompt(AiTaskKind task, const std::string &parameter) {
	switch (task) {
	case AiTaskKind::SUMMARIZE:
		return "Summarize the following text concisely. Return only the summary.";
	case AiTaskKind::SENTIMENT:
		return BuildTaskSystemPrompt(AiTaskKind::CLASSIFY, "positive, neutral, negative");
	case AiTaskKind::FIX_GRAMMAR:
		return "Fix grammar, spelling, and punctuation in the following text. Preserve the original meaning and "
		       "return only the corrected text.";
	case AiTaskKind::REDACT:
		return "Mask direct personal data, credentials, secrets, and payment identifiers in the following text. "
		       "Return only the redacted text.";
	case AiTaskKind::TRANSLATE:
		return "Translate the following text to " + parameter +
		       ". Preserve meaning and formatting. Return only the translation.";
	case AiTaskKind::CLASSIFY:
		return "Classify the following text into exactly one of these labels: " + parameter +
		       ". Return only the chosen label.";
	case AiTaskKind::EXTRACT:
		return "Extract the requested information from the following text. Return concise JSON when the request "
		       "asks for structured data.";
	case AiTaskKind::FILTER:
		return "Evaluate whether the following text matches the natural-language predicate. Return only true or "
		       "false.";
	default:
		throw InternalException("Unknown AI task kind");
	}
}

std::string BuildTaskUserPrompt(AiTaskKind task, const std::string &input, const std::string &parameter) {
	switch (task) {
	case AiTaskKind::EXTRACT:
		return "Extraction request:\n" + parameter + "\n\nText:\n" + input;
	case AiTaskKind::FILTER:
		return "Predicate:\n" + parameter + "\n\nText:\n" + input;
	default:
		return "Text:\n" + input;
	}
}

std::string BuildMultiLabelClassifySystemPrompt(const std::string &labels) {
	return "Classify the following text into zero or more of these labels: " + labels +
	       ". Return only a JSON array of chosen labels. Use [] when none apply.";
}

std::string BuildClassificationGuidance(const AiTaskBindData &bind_data) {
	std::string guidance;
	if (!bind_data.label_descriptions.empty()) {
		guidance += "\nLabel descriptions:\n" + bind_data.label_descriptions;
	}
	if (!bind_data.instructions.empty()) {
		guidance += "\nAdditional classification instructions:\n" + bind_data.instructions;
	}
	if (!bind_data.examples.empty()) {
		guidance += "\nExamples:\n" + bind_data.examples;
	}
	return guidance;
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

std::vector<std::string> ClassificationLabelsFromParameter(const std::string &parameter) {
	std::vector<std::string> labels;
	std::string error;
	if (duckdb_ai::ExtractJsonStringArray("[" + parameter + "]", labels, error) ||
	    duckdb_ai::ExtractJsonStringArray(parameter, labels, error)) {
		return labels;
	}
	for (idx_t start = 0; start <= parameter.size();) {
		auto separator = parameter.find(',', start);
		auto label =
		    TrimAscii(parameter.substr(start, separator == std::string::npos ? std::string::npos : separator - start));
		if (!label.empty()) {
			labels.push_back(std::move(label));
		}
		if (separator == std::string::npos) {
			break;
		}
		start = separator + 1;
	}
	return labels;
}

bool ValidateClassificationLabelSet(const std::vector<std::string> &labels, std::string &error) {
	if (labels.empty()) {
		error = "classification labels must not be empty";
		return false;
	}
	std::set<std::string> seen;
	for (auto &label : labels) {
		auto trimmed = TrimAscii(label);
		if (trimmed.empty()) {
			error = "classification labels must not contain empty values";
			return false;
		}
		if (!seen.insert(LowerAscii(trimmed)).second) {
			error = "classification labels must be unique";
			return false;
		}
	}
	return true;
}

bool CanonicalClassificationLabel(const std::string &parameter, const std::string &candidate, std::string &canonical,
                                  std::string &error) {
	auto labels = ClassificationLabelsFromParameter(parameter);
	if (!ValidateClassificationLabelSet(labels, error)) {
		return false;
	}
	auto normalized = TrimAscii(StripMarkdownJsonFence(candidate));
	std::vector<std::string> parsed_candidate;
	std::string parse_error;
	if (duckdb_ai::ExtractJsonStringArray("[" + normalized + "]", parsed_candidate, parse_error) &&
	    parsed_candidate.size() == 1) {
		normalized = parsed_candidate[0];
	}
	for (auto &label : labels) {
		if (LowerAscii(label) == LowerAscii(normalized)) {
			canonical = label;
			return true;
		}
	}
	error = "model returned a label outside the allowed label set: " + normalized;
	return false;
}

bool ValidateMultiClassificationLabels(const std::string &parameter, std::vector<std::string> &outputs,
                                       std::string &error) {
	auto labels = ClassificationLabelsFromParameter(parameter);
	if (!ValidateClassificationLabelSet(labels, error)) {
		return false;
	}
	std::set<std::string> seen;
	for (auto &output : outputs) {
		std::string canonical;
		if (!CanonicalClassificationLabel(parameter, output, canonical, error)) {
			return false;
		}
		auto normalized = LowerAscii(canonical);
		if (!seen.insert(normalized).second) {
			error = "model returned a duplicate classification label: " + canonical;
			return false;
		}
		output = std::move(canonical);
	}
	return true;
}

std::string StripMarkdownJsonFence(const std::string &text) {
	auto trimmed = TrimAscii(text);
	if (trimmed.size() < 3 || trimmed.compare(0, 3, "```") != 0) {
		return trimmed;
	}
	auto first_newline = trimmed.find('\n');
	if (first_newline == std::string::npos) {
		return trimmed;
	}
	auto last_fence = trimmed.rfind("```");
	if (last_fence == std::string::npos || last_fence <= first_newline) {
		return trimmed;
	}
	return TrimAscii(trimmed.substr(first_newline + 1, last_fence - first_newline - 1));
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

std::string BuildPromptSqlSystemPrompt(const std::string &schema_context) {
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
	return prompt;
}

std::string BuildPromptSqlPrompt(const std::string &question) {
	std::string prompt;
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
			auto included = include_specs.empty() || MatchesAnyTableSpec(info, include_specs);
			auto excluded = MatchesAnyTableSpec(info, exclude_specs);
			if (!included || excluded) {
				return;
			}
			info.comment = ValueToOptionalString(table.comment);
			info.sql = TableCreateSql(table);
			info.estimated_rows = EstimatedRows(context, table);
			info.columns = ExtractSchemaColumns(table);
			TryCollectSampleRows(context, table, info, options.sample_rows, allow_actual_samples);
			tables.push_back(std::move(info));
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
	auto parameter_reader =
	    OptionalStringReader(args, bind_data.required_args == 2 && !bind_data.parameter_is_label_list, 1);
	unique_ptr<StringListVectorReader> label_list_reader;
	if (bind_data.required_args == 2 && bind_data.parameter_is_label_list) {
		label_list_reader = make_uniq<StringListVectorReader>(args, 1);
	}
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	std::vector<ProviderStringJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		if (!input_reader.Read(row, input)) {
			result_validity.SetInvalid(row);
			continue;
		}
		std::string parameter;
		if (bind_data.required_args == 2) {
			auto read_parameter = bind_data.parameter_is_label_list ? label_list_reader->Read(row, parameter)
			                                                        : parameter_reader->Read(row, parameter);
			if (!read_parameter) {
				result_validity.SetInvalid(row);
				continue;
			}
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(state.GetContext(), bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderStringJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = options.fail_on_error;
			job.input = std::move(input);
			job.parameter = std::move(parameter);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}

	RunProviderJobs(jobs, [&](ProviderStringJob &job) {
		if (bind_data.task == AiTaskKind::REDACT &&
		    duckdb_ai::ResolveProvider(job.options).protocol == "privacy_filter") {
			job.output = duckdb_ai::Redact(job.input, job.options).text;
			return;
		}
		auto system_prompt = BuildTaskSystemPrompt(bind_data.task, job.parameter);
		if (bind_data.task == AiTaskKind::CLASSIFY) {
			system_prompt += BuildClassificationGuidance(bind_data);
		}
		AppendSystemPrompt(job.options, system_prompt);
		auto prompt = BuildTaskUserPrompt(bind_data.task, job.input, job.parameter);
		job.output = duckdb_ai::Complete(prompt, job.options).text;
		if (bind_data.task == AiTaskKind::CLASSIFY) {
			std::string canonical;
			std::string error;
			if (!CanonicalClassificationLabel(job.parameter, job.output, canonical, error)) {
				throw InvalidInputException("ai_classify %s", error);
			}
			job.output = std::move(canonical);
		}
	});

	for (auto &job : jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		result_data[job.row] = StringVector::AddString(result, job.output);
	}
}

void AiClassifyLabelsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiTaskBindData>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<list_entry_t>(result);
	auto &result_validity = FlatVector::Validity(result);
	for (idx_t row = 0; row < args.size(); row++) {
		result_data[row] = list_entry_t(0, 0);
	}
	StringVectorReader input_reader(args, 0);
	auto label_reader = OptionalStringReader(args, !bind_data.parameter_is_label_list, 1);
	unique_ptr<StringListVectorReader> label_list_reader;
	if (bind_data.parameter_is_label_list) {
		label_list_reader = make_uniq<StringListVectorReader>(args, 1);
	}
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);

	std::vector<ProviderStringListJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		std::string labels;
		auto read_labels =
		    bind_data.parameter_is_label_list ? label_list_reader->Read(row, labels) : label_reader->Read(row, labels);
		if (!input_reader.Read(row, input) || !read_labels) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(state.GetContext(), bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderStringListJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = options.fail_on_error;
			job.input = std::move(input);
			job.parameter = std::move(labels);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}

	RunProviderJobs(jobs, [&](ProviderStringListJob &job) {
		AppendSystemPrompt(job.options,
		                   BuildMultiLabelClassifySystemPrompt(job.parameter) + BuildClassificationGuidance(bind_data));
		auto prompt = BuildTaskUserPrompt(AiTaskKind::CLASSIFY, job.input, job.parameter);
		auto output = StripMarkdownJsonFence(duckdb_ai::Complete(prompt, job.options).text);
		std::string error;
		if (!duckdb_ai::ExtractJsonStringArray(output, job.output, error)) {
			throw InvalidInputException("ai_classify_labels expected a JSON array of strings: %s", error);
		}
		if (!ValidateMultiClassificationLabels(job.parameter, job.output, error)) {
			throw InvalidInputException("ai_classify_labels %s", error);
		}
	});

	for (auto &job : jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		result_data[job.row].offset = ListVector::GetListSize(result);
		result_data[job.row].length = job.output.size();
		ListVector::Reserve(result, result_data[job.row].offset + job.output.size());
		for (auto &label : job.output) {
			ListVector::PushBack(result, Value(label));
		}
	}
}

LogicalType AiClassifyResultReturnType() {
	child_list_t<LogicalType> children;
	children.emplace_back("value", LogicalType::LIST(LogicalType::VARCHAR));
	children.emplace_back("error", LogicalType::VARCHAR);
	children.emplace_back("metadata", LogicalType::VARCHAR);
	return LogicalType::STRUCT(std::move(children));
}

unique_ptr<FunctionData> AiClassifyResultBind(ClientContext &context, ScalarFunction &bound_function,
                                              vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = AiClassifyLabelsBind(context, bound_function, arguments);
	auto &task_bind_data = bind_data->Cast<AiTaskBindData>();
	task_bind_data.options.on_error = "capture";
	task_bind_data.options.fail_on_error = false;
	bound_function.SetReturnType(AiClassifyResultReturnType());
	return bind_data;
}

std::string JsonEscapeSqlText(const std::string &input) {
	std::string output;
	for (auto c : input) {
		switch (c) {
		case '\\':
			output += "\\\\";
			break;
		case '"':
			output += "\\\"";
			break;
		case '\n':
			output += "\\n";
			break;
		case '\r':
			output += "\\r";
			break;
		case '\t':
			output += "\\t";
			break;
		default:
			output.push_back(c);
			break;
		}
	}
	return output;
}

Value AiClassifyResultValue(const LogicalType &result_type, const std::vector<std::string> &labels,
                            const std::string &error, const std::string &metadata) {
	vector<Value> children;
	if (error.empty()) {
		vector<Value> label_values;
		for (auto &label : labels) {
			label_values.emplace_back(label);
		}
		children.push_back(Value::LIST(LogicalType::VARCHAR, std::move(label_values)));
		children.emplace_back(LogicalType::VARCHAR);
		children.emplace_back(metadata);
	} else {
		children.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
		children.emplace_back(error);
		children.push_back(metadata.empty() ? Value(LogicalType::VARCHAR) : Value(metadata));
	}
	return Value::STRUCT(result_type, std::move(children));
}

void AiClassifyResultFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<AiTaskBindData>();
	auto result_type = AiClassifyResultReturnType();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader input_reader(args, 0);
	auto label_reader = OptionalStringReader(args, !bind_data.parameter_is_label_list, 1);
	unique_ptr<StringListVectorReader> label_list_reader;
	if (bind_data.parameter_is_label_list) {
		label_list_reader = make_uniq<StringListVectorReader>(args, 1);
	}
	auto model_reader = OptionalStringReader(args, bind_data.has_model_arg, bind_data.model_index);
	auto provider_reader = OptionalStringReader(args, bind_data.has_provider_arg, bind_data.provider_index);
	std::vector<ProviderStringListJob> jobs;
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		std::string labels;
		auto read_labels =
		    bind_data.parameter_is_label_list ? label_list_reader->Read(row, labels) : label_reader->Read(row, labels);
		if (!input_reader.Read(row, input) || !read_labels) {
			result_validity.SetInvalid(row);
			continue;
		}
		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(state.GetContext(), bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}
		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderStringListJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = false;
			job.input = std::move(input);
			job.parameter = std::move(labels);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			result.SetValue(row,
			                AiClassifyResultValue(result_type, {}, ex.what(),
			                                      R"({"stage":"configuration","multi_label":true,"status":"error"})"));
		}
	}
	RunProviderJobs(jobs, [&](ProviderStringListJob &job) {
		AppendSystemPrompt(job.options,
		                   BuildMultiLabelClassifySystemPrompt(job.parameter) + BuildClassificationGuidance(bind_data));
		auto output = StripMarkdownJsonFence(
		    duckdb_ai::Complete(BuildTaskUserPrompt(AiTaskKind::CLASSIFY, job.input, job.parameter), job.options).text);
		std::string error;
		if (!duckdb_ai::ExtractJsonStringArray(output, job.output, error)) {
			throw InvalidInputException("ai_classify_result expected a JSON array of strings: %s", error);
		}
		if (!ValidateMultiClassificationLabels(job.parameter, job.output, error)) {
			throw InvalidInputException("ai_classify_result %s", error);
		}
	});
	for (auto &job : jobs) {
		std::string metadata;
		try {
			auto config = duckdb_ai::ResolveProvider(job.options);
			metadata = "{\"provider\":\"" + JsonEscapeSqlText(config.provider) + "\",\"model\":\"" +
			           JsonEscapeSqlText(config.model) + "\",\"multi_label\":true";
		} catch (...) {
			metadata = R"({"stage":"configuration","multi_label":true)";
		}
		if (job.exception) {
			metadata += ",\"status\":\"error\"}";
			result.SetValue(job.row, AiClassifyResultValue(result_type, {}, job.error_message, metadata));
			continue;
		}
		metadata += ",\"status\":\"ok\"}";
		result.SetValue(job.row, AiClassifyResultValue(result_type, job.output, "", metadata));
	}
}

std::vector<std::string> ReadClassifierLabels(ClientContext &context, Expression &expression) {
	if (expression.return_type.id() != LogicalTypeId::LIST) {
		throw BinderException("ai_build_classifier labels must be a constant VARCHAR[]");
	}
	auto value = EvaluateConstantOption(context, expression, "labels", LogicalType::LIST(LogicalType::VARCHAR));
	std::vector<std::string> labels;
	std::set<std::string> seen;
	for (auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw BinderException("ai_build_classifier labels must not contain NULL");
		}
		auto label_value = child;
		auto label = StringValue::Get(label_value.DefaultCastAs(LogicalType::VARCHAR));
		if (label.empty() || !seen.insert(LowerAscii(label)).second) {
			throw BinderException("ai_build_classifier labels must be non-empty and unique");
		}
		labels.push_back(std::move(label));
	}
	if (labels.size() < 2) {
		throw BinderException("ai_build_classifier requires at least two labels");
	}
	return labels;
}

unique_ptr<FunctionData> ClassifierBuildBind(ClientContext &context, AggregateFunction &function,
                                             vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() < 2) {
		throw BinderException("ai_build_classifier requires input text and a constant VARCHAR[] label list");
	}
	auto bind_data = make_uniq<ClassifierBuildBindData>();
	ApplySettings(context, bind_data->label_options, AiModelSettingKind::TASK);
	ApplySettings(context, bind_data->embedding_options, AiModelSettingKind::EMBEDDING);
	StampProviderFunction(bind_data->label_options, "ai_build_classifier_label");
	StampProviderFunction(bind_data->embedding_options, "ai_build_classifier_embed");
	duckdb_ai::AttachProviderRuntimeState(bind_data->label_options, context);
	duckdb_ai::AttachProviderRuntimeState(bind_data->embedding_options, context);
	bind_data->labels = ReadClassifierLabels(context, *arguments[1]);
	vector<idx_t> erase_indexes {1};
	for (idx_t i = 2; i < arguments.size(); i++) {
		auto alias = LowerAscii(arguments[i]->GetAlias());
		if (alias.empty()) {
			throw BinderException("ai_build_classifier options must use named arguments");
		}
		if (alias == "optimization") {
			auto value = LowerAscii(EvaluateConstantString(context, *arguments[i], alias));
			if (value != "minimize_cost") {
				throw BinderException("ai_build_classifier optimization must be minimize_cost");
			}
		} else if (alias == "sample_size") {
			auto value = EvaluateConstantOption(context, *arguments[i], alias, LogicalType::BIGINT);
			auto sample_size = BigIntValue::Get(value);
			if (sample_size < 8 || sample_size > 10000) {
				throw BinderException("ai_build_classifier sample_size must be between 8 and 10000");
			}
			bind_data->sample_size = NumericCast<idx_t>(sample_size);
		} else if (alias == "quality_threshold" || alias == "confidence_margin") {
			auto value = EvaluateConstantOption(context, *arguments[i], alias, LogicalType::DOUBLE);
			auto number = DoubleValue::Get(value);
			if (!std::isfinite(number) || number < 0 || number > 1) {
				throw BinderException("ai_build_classifier %s must be between 0 and 1", alias);
			}
			if (alias == "quality_threshold") {
				bind_data->quality_threshold = number;
			} else {
				bind_data->confidence_margin = number;
			}
		} else if (alias == "embedding_model" || alias == "embedding_provider" || alias == "embedding_profile") {
			auto value = EvaluateConstantString(context, *arguments[i], alias);
			if (alias == "embedding_model") {
				bind_data->embedding_options.model = value;
				bind_data->embedding_options.explicit_model = true;
			} else if (alias == "embedding_provider") {
				bind_data->embedding_options.provider = value;
				bind_data->embedding_options.explicit_provider = true;
			} else {
				bind_data->embedding_options.secret_name = value;
			}
		} else {
			ApplyNamedOption(context, bind_data->label_options, *arguments[i], alias);
		}
		erase_indexes.push_back(i);
	}
	ApplyAiProviderSecret(context, bind_data->label_options);
	ApplyAiProviderSecret(context, bind_data->embedding_options);
	for (idx_t offset = erase_indexes.size(); offset > 0; offset--) {
		Function::EraseArgument(function, arguments, erase_indexes[offset - 1]);
	}
	function.SetReturnType(LogicalType::VARCHAR);
	return std::move(bind_data);
}

void ClassifierEnsureCapacity(ClassifierBuildState &state, ArenaAllocator &allocator, idx_t required_size) {
	if (state.alloc_size >= required_size) {
		return;
	}
	auto new_size = state.alloc_size == 0 ? MaxValue<idx_t>(64, required_size) : state.alloc_size;
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

void ClassifierAppendSample(ClassifierBuildState &state, ArenaAllocator &allocator, const char *data, idx_t size,
                            idx_t sample_size) {
	state.row_count++;
	if (state.sample_count >= sample_size) {
		return;
	}
	ClassifierEnsureCapacity(state, allocator, state.size + sizeof(uint64_t) + size);
	auto length = static_cast<uint64_t>(size);
	memcpy(state.dataptr + state.size, &length, sizeof(uint64_t));
	state.size += sizeof(uint64_t);
	memcpy(state.dataptr + state.size, data, size);
	state.size += size;
	state.sample_count++;
}

std::vector<std::string> ClassifierSamples(const ClassifierBuildState &state) {
	std::vector<std::string> samples;
	idx_t offset = 0;
	while (offset + sizeof(uint64_t) <= state.size && samples.size() < state.sample_count) {
		uint64_t length;
		memcpy(&length, state.dataptr + offset, sizeof(uint64_t));
		offset += sizeof(uint64_t);
		if (length > state.size - offset) {
			throw InternalException("ai_build_classifier aggregate state is corrupt");
		}
		samples.emplace_back(state.dataptr + offset, NumericCast<idx_t>(length));
		offset += NumericCast<idx_t>(length);
	}
	return samples;
}

std::string JoinClassifierLabels(const std::vector<std::string> &labels) {
	std::string result;
	for (idx_t i = 0; i < labels.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += labels[i];
	}
	return result;
}

idx_t ClassifierLabelIndex(const std::vector<std::string> &labels, const std::string &input) {
	auto normalized = LowerAscii(TrimAscii(StripMarkdownJsonFence(input)));
	if (normalized.size() >= 2 && normalized.front() == '"' && normalized.back() == '"') {
		normalized = normalized.substr(1, normalized.size() - 2);
	}
	for (idx_t i = 0; i < labels.size(); i++) {
		if (LowerAscii(labels[i]) == normalized) {
			return i;
		}
	}
	return DConstants::INVALID_INDEX;
}

std::string BuildClassifierArtifact(const ClassifierBuildBindData &bind_data, const ClassifierBuildState &state) {
	auto samples = ClassifierSamples(state);
	std::vector<ProviderStringJob> label_jobs;
	for (idx_t i = 0; i < samples.size(); i++) {
		ProviderStringJob job;
		job.row = i;
		job.input = samples[i];
		job.options = bind_data.label_options;
		job.fail_on_error = bind_data.label_options.fail_on_error;
		label_jobs.push_back(std::move(job));
	}
	auto labels_text = JoinClassifierLabels(bind_data.labels);
	RunProviderJobs(label_jobs, [&](ProviderStringJob &job) {
		AppendSystemPrompt(job.options, BuildTaskSystemPrompt(AiTaskKind::CLASSIFY, labels_text));
		job.output =
		    duckdb_ai::Complete(BuildTaskUserPrompt(AiTaskKind::CLASSIFY, job.input, labels_text), job.options).text;
	});
	std::vector<std::string> labelled_samples;
	std::vector<idx_t> labelled_classes;
	for (auto &job : label_jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			continue;
		}
		auto label_index = ClassifierLabelIndex(bind_data.labels, job.output);
		if (label_index == DConstants::INVALID_INDEX) {
			if (job.fail_on_error) {
				throw InvalidInputException("ai_build_classifier received an unknown label: %s", job.output);
			}
			continue;
		}
		labelled_samples.push_back(std::move(job.input));
		labelled_classes.push_back(label_index);
	}
	if (labelled_samples.empty()) {
		throw InvalidInputException("ai_build_classifier could not label any sampled rows");
	}
	auto embeddings = duckdb_ai::EmbedMany(labelled_samples, bind_data.embedding_options);
	auto dimensions = embeddings.empty() ? 0 : embeddings[0].values.size();
	std::vector<idx_t> class_totals(bind_data.labels.size(), 0);
	for (auto label : labelled_classes) {
		class_totals[label]++;
	}
	std::vector<bool> validation_rows(embeddings.size(), false);
	for (idx_t i = 0; i < embeddings.size(); i++) {
		validation_rows[i] = i % 5 == 0 && class_totals[labelled_classes[i]] > 1;
	}
	std::vector<std::vector<double>> centroids(bind_data.labels.size(), std::vector<double>(dimensions, 0));
	std::vector<idx_t> counts(bind_data.labels.size(), 0);
	for (idx_t i = 0; i < embeddings.size(); i++) {
		if (embeddings[i].values.size() != dimensions) {
			throw InvalidInputException("ai_build_classifier embedding dimensions are inconsistent");
		}
		if (validation_rows[i]) {
			continue;
		}
		counts[labelled_classes[i]]++;
		for (idx_t dimension = 0; dimension < dimensions; dimension++) {
			centroids[labelled_classes[i]][dimension] += embeddings[i].values[dimension];
		}
	}
	for (idx_t label = 0; label < centroids.size(); label++) {
		if (counts[label] == 0) {
			continue;
		}
		for (auto &value : centroids[label]) {
			value /= static_cast<double>(counts[label]);
		}
	}
	idx_t correct = 0;
	idx_t evaluated = 0;
	for (idx_t i = 0; i < embeddings.size(); i++) {
		if (!validation_rows[i]) {
			continue;
		}
		double best_score = -std::numeric_limits<double>::infinity();
		idx_t best_label = DConstants::INVALID_INDEX;
		for (idx_t label = 0; label < centroids.size(); label++) {
			if (counts[label] == 0) {
				continue;
			}
			auto score = CosineSimilarity(embeddings[i].values, centroids[label]);
			if (score > best_score) {
				best_score = score;
				best_label = label;
			}
		}
		correct += best_label == labelled_classes[i] ? 1 : 0;
		evaluated++;
	}
	auto accuracy = evaluated == 0 ? 0 : static_cast<double>(correct) / static_cast<double>(evaluated);
	auto all_classes_present = std::all_of(counts.begin(), counts.end(), [](idx_t count) { return count > 0; });
	auto usable = all_classes_present && accuracy >= bind_data.quality_threshold;
	auto embedding_config = duckdb_ai::ResolveProvider(bind_data.embedding_options);
	std::ostringstream artifact;
	artifact << std::setprecision(17);
	artifact << "{\"version\":1,\"optimization\":\"minimize_cost\",\"usable\":" << (usable ? "true" : "false")
	         << ",\"accuracy\":" << accuracy << ",\"quality_threshold\":" << bind_data.quality_threshold
	         << ",\"validation_count\":" << evaluated << ",\"confidence_margin\":" << bind_data.confidence_margin
	         << ",\"sample_count\":" << labelled_samples.size() << ",\"total_count\":" << state.row_count
	         << ",\"embedding\":{\"provider\":\"" << JsonEscapeSqlText(embedding_config.provider) << "\",\"model\":\""
	         << JsonEscapeSqlText(embedding_config.model) << "\",\"profile\":\""
	         << JsonEscapeSqlText(bind_data.embedding_options.secret_name) << "\",\"base_url\":\""
	         << JsonEscapeSqlText(bind_data.embedding_options.base_url) << "\",\"options\":\""
	         << JsonEscapeSqlText(bind_data.embedding_options.model_options) << "\"},\"labels\":[";
	for (idx_t label = 0; label < bind_data.labels.size(); label++) {
		if (label > 0) {
			artifact << ',';
		}
		artifact << '"' << JsonEscapeSqlText(bind_data.labels[label]) << '"';
	}
	artifact << "],\"centroids\":[";
	for (idx_t label = 0; label < centroids.size(); label++) {
		if (label > 0) {
			artifact << ',';
		}
		artifact << '[';
		for (idx_t dimension = 0; dimension < centroids[label].size(); dimension++) {
			if (dimension > 0) {
				artifact << ',';
			}
			artifact << centroids[label][dimension];
		}
		artifact << ']';
	}
	artifact << "]}";
	return artifact.str();
}

struct ClassifierBuildOperation {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.size = 0;
		state.alloc_size = 0;
		state.sample_count = 0;
		state.row_count = 0;
		state.dataptr = nullptr;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		auto &bind_data = unary_input.input.bind_data->Cast<ClassifierBuildBindData>();
		ClassifierAppendSample(state, unary_input.input.allocator, input.GetData(), input.GetSize(),
		                       bind_data.sample_size);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggregate_input) {
		auto &bind_data = aggregate_input.bind_data->Cast<ClassifierBuildBindData>();
		auto samples = ClassifierSamples(source);
		target.row_count += source.row_count;
		for (auto &sample : samples) {
			if (target.sample_count >= bind_data.sample_size) {
				break;
			}
			auto before = target.row_count;
			ClassifierAppendSample(target, aggregate_input.allocator, sample.data(), sample.size(),
			                       bind_data.sample_size);
			target.row_count = before;
		}
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.row_count == 0) {
			finalize_data.ReturnNull();
			return;
		}
		auto &bind_data = finalize_data.input.bind_data->Cast<ClassifierBuildBindData>();
		try {
			auto artifact = BuildClassifierArtifact(bind_data, state);
			target = finalize_data.ReturnString(string_t(artifact));
		} catch (std::exception &) {
			if (bind_data.label_options.fail_on_error) {
				throw;
			}
			finalize_data.ReturnNull();
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

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

	std::vector<ProviderBoolJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
	for (idx_t row = 0; row < args.size(); row++) {
		std::string input;
		std::string predicate;
		if (!input_reader.Read(row, input) || !predicate_reader.Read(row, predicate)) {
			result_validity.SetInvalid(row);
			continue;
		}

		duckdb_ai::CompletionOptions options;
		if (!ReadRuntimeOptions(state.GetContext(), bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderBoolJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = options.fail_on_error;
			job.input = std::move(input);
			job.parameter = std::move(predicate);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}

	RunProviderJobs(jobs, [&](ProviderBoolJob &job) {
		AppendSystemPrompt(job.options, BuildTaskSystemPrompt(bind_data.task, job.parameter));
		auto prompt = BuildTaskUserPrompt(bind_data.task, job.input, job.parameter);
		auto output = duckdb_ai::Complete(prompt, job.options).text;
		bool parsed = false;
		if (!ParseAiBooleanResult(output, parsed)) {
			throw InvalidInputException("ai_filter expected true or false output, got: %s", output);
		}
		job.output = parsed;
	});

	for (auto &job : jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		result_data[job.row] = job.output;
	}
}

unique_ptr<FunctionData> AiCountTokensBind(ClientContext &context, ScalarFunction &bound_function,
                                           vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = AiCompletionBindInternal(context, bound_function, arguments, false);
	bound_function.SetReturnType(LogicalType::BIGINT);
	return bind_data;
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
		result_data[row] = duckdb_ai::EstimateTokenCount(input);
	}
}

double NumericArgValue(DataChunk &args, idx_t column, idx_t row, bool &is_null) {
	auto value = args.data[column].GetValue(row);
	if (value.IsNull()) {
		is_null = true;
		return 0;
	}
	return DoubleValue::Get(value.DefaultCastAs(LogicalType::DOUBLE));
}

int64_t RecommendedBatchSize(double input_tokens_per_row, double max_output_tokens_per_row,
                             double token_limit_per_minute, double request_limit_per_minute, double safety_factor) {
	if (!std::isfinite(input_tokens_per_row) || input_tokens_per_row < 0) {
		throw InvalidInputException(
		    "ai_recommended_batch_size input_tokens_per_row must be greater than or equal to 0");
	}
	if (!std::isfinite(max_output_tokens_per_row) || max_output_tokens_per_row < 0) {
		throw InvalidInputException(
		    "ai_recommended_batch_size max_output_tokens_per_row must be greater than or equal to 0");
	}
	if (!std::isfinite(token_limit_per_minute) || token_limit_per_minute < 0) {
		throw InvalidInputException(
		    "ai_recommended_batch_size token_limit_per_minute must be greater than or equal to 0");
	}
	if (!std::isfinite(request_limit_per_minute) || request_limit_per_minute < 0) {
		throw InvalidInputException(
		    "ai_recommended_batch_size request_limit_per_minute must be greater than or equal to 0");
	}
	if (!std::isfinite(safety_factor) || safety_factor <= 0 || safety_factor > 1) {
		throw InvalidInputException("ai_recommended_batch_size safety_factor must be greater than 0 and at most 1");
	}
	if (token_limit_per_minute <= 0 && request_limit_per_minute <= 0) {
		throw InvalidInputException("ai_recommended_batch_size requires token_limit_per_minute or "
		                            "request_limit_per_minute to be greater than 0");
	}

	auto tokens_per_row = std::max(1.0, input_tokens_per_row + max_output_tokens_per_row);
	auto token_rows = std::numeric_limits<double>::infinity();
	if (token_limit_per_minute > 0) {
		token_rows = std::floor((token_limit_per_minute * safety_factor) / tokens_per_row);
	}
	auto request_rows = std::numeric_limits<double>::infinity();
	if (request_limit_per_minute > 0) {
		request_rows = std::floor(request_limit_per_minute * safety_factor);
	}
	auto recommended = std::min(token_rows, request_rows);
	if (!std::isfinite(recommended)) {
		throw InvalidInputException("ai_recommended_batch_size requires token_limit_per_minute or "
		                            "request_limit_per_minute to be greater than 0");
	}
	return std::max<int64_t>(1, static_cast<int64_t>(recommended));
}

void AiRecommendedBatchSizeFunction(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<int64_t>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t row = 0; row < args.size(); row++) {
		bool is_null = false;
		auto input_tokens_per_row = NumericArgValue(args, 0, row, is_null);
		auto max_output_tokens_per_row = NumericArgValue(args, 1, row, is_null);
		auto token_limit_per_minute = NumericArgValue(args, 2, row, is_null);
		auto request_limit_per_minute = args.ColumnCount() >= 4 ? NumericArgValue(args, 3, row, is_null) : 0;
		auto safety_factor = args.ColumnCount() >= 5 ? NumericArgValue(args, 4, row, is_null) : 0.8;
		if (is_null) {
			result_validity.SetInvalid(row);
			continue;
		}
		result_data[row] = RecommendedBatchSize(input_tokens_per_row, max_output_tokens_per_row, token_limit_per_minute,
		                                        request_limit_per_minute, safety_factor);
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

bool VerifySqlBinds(ClientContext &context, const std::string &sql, std::string &error) {
	try {
		Parser parser(context.GetParserOptions());
		parser.ParseQuery(sql);
		if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
			error = "expected exactly one SELECT statement";
			return false;
		}
		auto binder = Binder::CreateBinder(context);
		binder->Bind(*parser.statements[0]);
		return true;
	} catch (std::exception &ex) {
		ErrorData error_data(ex);
		error = error_data.Message();
		return false;
	}
}

void RequireReadOnlySql(const std::string &sql, const std::string &function_name) {
	std::string error;
	if (!ValidateReadOnlySql(sql, error)) {
		throw InvalidInputException("%s requires a single read-only SELECT statement: %s", function_name, error);
	}
}

std::string GeneratePromptSql(const std::string &question, const std::string &schema_context,
                              const duckdb_ai::CompletionOptions &options) {
	auto request_options = options;
	AppendSystemPrompt(request_options, BuildPromptSqlSystemPrompt(schema_context));
	auto prompt = BuildPromptSqlPrompt(question);
	auto generated_sql = StripMarkdownSqlFence(duckdb_ai::Complete(prompt, request_options).text);
	std::string error;
	if (!ValidateReadOnlySql(generated_sql, error)) {
		throw InvalidInputException("AI generated SQL is not a single read-only SELECT statement: %s", error);
	}
	return generated_sql;
}

std::string BuildPromptExplainSystemPrompt(const std::string &schema_context) {
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
	return prompt;
}

std::string BuildPromptExplainPrompt(const std::string &sql) {
	std::string prompt;
	prompt += "\nSQL query:\n";
	prompt += sql;
	return prompt;
}

std::string BuildPromptFixupSystemPrompt(const std::string &schema_context) {
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
	return prompt;
}

std::string BuildPromptFixupPrompt(const std::string &sql, const std::string &error_message = "",
                                   const std::string &question = "") {
	std::string prompt;
	if (!question.empty()) {
		prompt += "\nOriginal question:\n";
		prompt += question;
		prompt += "\n";
	}
	if (!error_message.empty()) {
		prompt += "\nError message:\n";
		prompt += error_message;
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

std::string BuildPromptFixLineSystemPrompt(const std::string &schema_context) {
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
	return prompt;
}

std::string BuildPromptFixLinePrompt(const std::string &sql, const std::string &error_message, idx_t line_number) {
	std::string prompt;
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

std::string ReplaceSqlLine(const std::string &sql, idx_t one_based_line_number, const std::string &replacement) {
	std::string output;
	idx_t current_line = 1;
	size_t line_start = 0;
	while (line_start <= sql.size()) {
		auto line_end = sql.find('\n', line_start);
		auto has_newline = line_end != std::string::npos;
		auto line = sql.substr(line_start, has_newline ? line_end - line_start : std::string::npos);
		if (!output.empty()) {
			output += "\n";
		}
		output += current_line == one_based_line_number ? replacement : line;
		if (!has_newline) {
			break;
		}
		line_start = line_end + 1;
		current_line++;
	}
	return output;
}

std::string GeneratePromptFixupSql(const std::string &sql, const std::string &schema_context,
                                   const duckdb_ai::CompletionOptions &options, const std::string &error_message = "",
                                   const std::string &question = "") {
	auto request_options = options;
	AppendSystemPrompt(request_options, BuildPromptFixupSystemPrompt(schema_context));
	auto prompt = BuildPromptFixupPrompt(sql, error_message, question);
	auto fixed_sql = StripMarkdownSqlFence(duckdb_ai::Complete(prompt, request_options).text);
	std::string error;
	if (!ValidateReadOnlySql(fixed_sql, error)) {
		throw InvalidInputException("AI corrected SQL is not a single read-only SELECT statement: %s", error);
	}
	return fixed_sql;
}

std::string RepairGeneratedSql(ClientContext &context, const std::string &question, std::string generated_sql,
                               const std::string &schema_context, const duckdb_ai::CompletionOptions &options,
                               int64_t fix_attempts, const std::string &usage_event) {
	if (fix_attempts <= 0) {
		return generated_sql;
	}
	std::string bind_error;
	for (int64_t attempt = 0; attempt < fix_attempts; attempt++) {
		if (VerifySqlBinds(context, generated_sql, bind_error)) {
			return generated_sql;
		}
		duckdb_ai::RecordLocalUsageEvent(&context, usage_event, static_cast<int64_t>(generated_sql.size()),
		                                 static_cast<int64_t>(bind_error.size()));
		generated_sql = GeneratePromptFixupSql(generated_sql, schema_context, options, bind_error, question);
	}
	if (!VerifySqlBinds(context, generated_sql, bind_error)) {
		throw InvalidInputException("AI generated SQL still fails to bind after %lld fix attempt(s): %s", fix_attempts,
		                            bind_error);
	}
	return generated_sql;
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
                                 PromptSchemaOptions &schema_options, int64_t &fix_attempts, const std::string &name,
                                 const Value &value_p) {
	if (value_p.IsNull()) {
		throw BinderException("ai_query_data option \"%s\" must not be NULL", name);
	}
	auto value = value_p;
	if (name == "schema_context" || name == "schema") {
		schema_context = StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
		return;
	}
	if (name == "fix_attempts") {
		fix_attempts = ReadFixAttemptsValue(value, "ai_query_data");
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

PromptQueryCacheState &PromptQueryCache(ClientContext &context) {
	return *ObjectCache::GetObjectCache(context).GetOrCreate<PromptQueryCacheState>(
	    PromptQueryCacheState::ObjectType());
}

void AppendPromptQueryCacheKeyPart(std::string &key, const std::string &value) {
	key += std::to_string(value.size());
	key += ":";
	key += value;
	key += "\n";
}

void AppendPromptQueryCacheKeyPart(std::string &key, int64_t value) {
	AppendPromptQueryCacheKeyPart(key, std::to_string(value));
}

void AppendPromptQueryCacheKeyPart(std::string &key, double value) {
	std::ostringstream out;
	out.precision(17);
	out << value;
	AppendPromptQueryCacheKeyPart(key, out.str());
}

std::string PromptQueryCacheKey(const std::string &question, const std::string &schema_context,
                                const duckdb_ai::CompletionOptions &options) {
	std::string key;
	AppendPromptQueryCacheKeyPart(key, "ai_query_data_v1");
	AppendPromptQueryCacheKeyPart(key, question);
	AppendPromptQueryCacheKeyPart(key, schema_context);
	AppendPromptQueryCacheKeyPart(key, options.provider);
	AppendPromptQueryCacheKeyPart(key, options.model);
	AppendPromptQueryCacheKeyPart(key, options.base_url);
	AppendPromptQueryCacheKeyPart(key, options.system_prompt);
	AppendPromptQueryCacheKeyPart(key, options.response_format);
	AppendPromptQueryCacheKeyPart(key, options.response_schema);
	AppendPromptQueryCacheKeyPart(key, options.has_temperature ? int64_t(1) : int64_t(0));
	if (options.has_temperature) {
		AppendPromptQueryCacheKeyPart(key, options.temperature);
	}
	AppendPromptQueryCacheKeyPart(key, options.has_max_tokens ? int64_t(1) : int64_t(0));
	if (options.has_max_tokens) {
		AppendPromptQueryCacheKeyPart(key, options.max_tokens);
	}
	return key;
}

void RemovePromptQueryCacheOrderEntry(PromptQueryCacheState &cache, const std::string &cache_key) {
	auto entry = std::find(cache.recency_order.begin(), cache.recency_order.end(), cache_key);
	if (entry != cache.recency_order.end()) {
		cache.recency_order.erase(entry);
	}
}

bool TryGetPromptQueryCachedSql(ClientContext &context, const std::string &cache_key, std::string &generated_sql) {
	auto &cache = PromptQueryCache(context);
	std::lock_guard<std::mutex> lock(cache.mutex);
	auto entry = cache.generated_sql.find(cache_key);
	if (entry == cache.generated_sql.end()) {
		return false;
	}
	generated_sql = entry->second;
	RemovePromptQueryCacheOrderEntry(cache, cache_key);
	cache.recency_order.push_back(cache_key);
	return true;
}

void StorePromptQueryCachedSql(ClientContext &context, const std::string &cache_key, std::string generated_sql) {
	auto &cache = PromptQueryCache(context);
	std::lock_guard<std::mutex> lock(cache.mutex);
	RemovePromptQueryCacheOrderEntry(cache, cache_key);
	cache.generated_sql[cache_key] = std::move(generated_sql);
	cache.recency_order.push_back(cache_key);
	while (cache.generated_sql.size() > MAX_PROMPT_QUERY_CACHE_ENTRIES && !cache.recency_order.empty()) {
		auto oldest = std::move(cache.recency_order.front());
		cache.recency_order.pop_front();
		cache.generated_sql.erase(oldest);
	}
}

void ClearPromptQueryCache(ClientContext &context) {
	auto &cache = PromptQueryCache(context);
	std::lock_guard<std::mutex> lock(cache.mutex);
	cache.generated_sql.clear();
	cache.recency_order.clear();
}

unique_ptr<TableRef> PromptQueryBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto options = duckdb_ai::CompletionOptions();
	ApplySettings(context, options, AiModelSettingKind::SQL_ASSISTANT);
	StampProviderFunction(options, "ai_query_data");
	duckdb_ai::AttachProviderRuntimeState(options, context);
	auto question = RequiredTableStringInput(input, 0, "question");
	auto schema_context = OptionalTableStringInput(input, 1);
	PromptSchemaOptions schema_options;
	int64_t fix_attempts = 0;
	if (input.inputs.size() > 2) {
		options.model = OptionalTableStringInput(input, 2);
	}
	if (input.inputs.size() > 3) {
		options.provider = OptionalTableStringInput(input, 3);
	}
	for (auto &named_parameter : input.named_parameters) {
		ApplyPromptQueryValueOption(options, schema_context, schema_options, fix_attempts,
		                            LowerAscii(named_parameter.first), named_parameter.second);
	}
	if (schema_context.empty()) {
		schema_context = BuildPromptSchemaContext(context, schema_options);
	}

	try {
		ApplyAiProviderSecret(context, options);
		auto cache_key = PromptQueryCacheKey(question, schema_context, options);
		std::string generated_sql;
		if (TryGetPromptQueryCachedSql(context, cache_key, generated_sql)) {
			std::string bind_error;
			if (fix_attempts <= 0 || VerifySqlBinds(context, generated_sql, bind_error)) {
				duckdb_ai::RecordLocalUsageEvent(&context, "ai_query_data_cache_hit",
				                                 static_cast<int64_t>(question.size()),
				                                 static_cast<int64_t>(generated_sql.size()));
				return ParseReadOnlySubquery(generated_sql, context.GetParserOptions());
			}
			// the cached SQL no longer binds (e.g. the schema changed): fall through and regenerate
		}
		generated_sql = GeneratePromptSql(question, schema_context, options);
		generated_sql = RepairGeneratedSql(context, question, std::move(generated_sql), schema_context, options,
		                                   fix_attempts, "ai_query_data_fix_attempt");
		StorePromptQueryCachedSql(context, cache_key, generated_sql);
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

	std::vector<ProviderStringJob> jobs;
	jobs.reserve(args.size());
	std::vector<SecretResolutionCacheEntry> secret_cache;
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
		if (!ReadRuntimeOptions(state.GetContext(), bind_data.options, bind_data.has_model_arg, model_reader.get(),
		                        bind_data.has_provider_arg, provider_reader.get(), row, options)) {
			result_validity.SetInvalid(row);
			continue;
		}

		try {
			ApplyAiProviderSecretCached(state.GetContext(), options, secret_cache);
			ProviderStringJob job;
			job.row = row;
			job.options = options;
			job.fail_on_error = options.fail_on_error;
			job.input = std::move(question);
			job.parameter = std::move(schema_context);
			jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			if (options.fail_on_error) {
				throw;
			}
			result_validity.SetInvalid(row);
		}
	}

	RunProviderJobs(
	    jobs, [&](ProviderStringJob &job) { job.output = GeneratePromptSql(job.input, job.parameter, job.options); });

	if (bind_data.fix_attempts > 0) {
		// bind verification needs the client context, so repair runs serially after the parallel generation
		for (auto &job : jobs) {
			if (job.exception || !job.completed) {
				continue;
			}
			try {
				job.output = RepairGeneratedSql(state.GetContext(), job.input, std::move(job.output), job.parameter,
				                                job.options, bind_data.fix_attempts, "ai_sql_fix_attempt");
			} catch (std::exception &) {
				job.exception = std::current_exception();
			}
		}
	}

	for (auto &job : jobs) {
		if (job.exception) {
			if (job.fail_on_error) {
				std::rethrow_exception(job.exception);
			}
			result_validity.SetInvalid(job.row);
			continue;
		}
		if (!job.completed) {
			result_validity.SetInvalid(job.row);
			continue;
		}
		result_data[job.row] = StringVector::AddString(result, job.output);
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

int64_t ReadFixAttemptsValue(const Value &value_p, const std::string &function_name) {
	if (value_p.IsNull()) {
		throw BinderException("%s fix_attempts must not be NULL", function_name);
	}
	auto value = value_p;
	auto fix_attempts = BigIntValue::Get(value.DefaultCastAs(LogicalType::BIGINT));
	if (fix_attempts < 0 || fix_attempts > MAX_SQL_FIX_ATTEMPTS) {
		throw BinderException("%s fix_attempts must be between 0 and %lld", function_name, MAX_SQL_FIX_ATTEMPTS);
	}
	return fix_attempts;
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
	duckdb_ai::RecordLocalUsageEvent(&context, "ai_schema_prompt", 0, static_cast<int64_t>(summary.size()));
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
		if (bind_data.kind != PromptAssistantKind::FIX_LINE && bind_data.kind != PromptAssistantKind::FIXUP) {
			throw BinderException("%s does not support an error option", function_name);
		}
		if (has_error) {
			throw BinderException("%s error cannot be supplied more than once", function_name);
		}
		bind_data.error_message = StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
		has_error = true;
		return;
	}
	if (name == "fix_attempts") {
		if (bind_data.kind != PromptAssistantKind::FIXUP) {
			throw BinderException("%s only supports fix_attempts with mode query", function_name);
		}
		bind_data.fix_attempts = ReadFixAttemptsValue(value, function_name);
		return;
	}
	if (name == "mode") {
		if (function_name != "ai_fix_sql") {
			throw BinderException("%s does not support a mode option", function_name);
		}
		auto mode = LowerAscii(StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR)));
		if (mode != "query" && mode != "full" && mode != "line") {
			throw BinderException("%s mode must be query, full, or line", function_name);
		}
		return;
	}
	if (!ApplyCompletionValueOption(bind_data.options, function_name, name, value_p, true, false)) {
		throw BinderException("Unsupported %s option \"%s\"", function_name, name);
	}
}

PromptAssistantKind PromptFixSqlKindFromInput(TableFunctionBindInput &input) {
	bool has_explicit_mode = false;
	PromptAssistantKind kind = PromptAssistantKind::FIXUP;
	for (auto &named_parameter : input.named_parameters) {
		auto name = LowerAscii(named_parameter.first);
		if (name == "mode") {
			if (named_parameter.second.IsNull()) {
				throw BinderException("ai_fix_sql option \"mode\" must not be NULL");
			}
			auto value = named_parameter.second;
			auto mode = LowerAscii(StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR)));
			if (mode == "line") {
				kind = PromptAssistantKind::FIX_LINE;
			} else if (mode == "query" || mode == "full") {
				kind = PromptAssistantKind::FIXUP;
			} else {
				throw BinderException("ai_fix_sql mode must be query, full, or line");
			}
			has_explicit_mode = true;
			continue;
		}
		if (!has_explicit_mode && name == "error") {
			kind = PromptAssistantKind::FIX_LINE;
		}
	}
	if (!has_explicit_mode && input.inputs.size() > 1) {
		kind = PromptAssistantKind::FIX_LINE;
	}
	return kind;
}

unique_ptr<FunctionData> PromptAssistantBindInternal(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names,
                                                     PromptAssistantKind kind, const std::string &function_name) {
	auto bind_data = make_uniq<PromptAssistantBindData>(kind);
	ApplySettings(context, bind_data->options, AiModelSettingKind::SQL_ASSISTANT);
	StampProviderFunction(bind_data->options, function_name);
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
	return PromptAssistantBindInternal(context, input, return_types, names, PromptFixSqlKindFromInput(input),
	                                   "ai_fix_sql");
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
		duckdb_ai::AttachProviderRuntimeState(options, context);
		ApplyAiProviderSecret(context, options);
		if (bind_data.kind == PromptAssistantKind::EXPLAIN) {
			RequireReadOnlySql(bind_data.sql, "ai_explain_sql");
			AppendSystemPrompt(options, BuildPromptExplainSystemPrompt(bind_data.schema_context));
			auto prompt = BuildPromptExplainPrompt(bind_data.sql);
			auto explanation = duckdb_ai::Complete(prompt, options).text;
			output.SetValue(0, 0, Value(explanation));
		} else if (bind_data.kind == PromptAssistantKind::FIXUP) {
			auto fixed_sql =
			    GeneratePromptFixupSql(bind_data.sql, bind_data.schema_context, options, bind_data.error_message);
			fixed_sql = RepairGeneratedSql(context, "", std::move(fixed_sql), bind_data.schema_context, options,
			                               bind_data.fix_attempts, "ai_fix_sql_fix_attempt");
			output.SetValue(0, 0, Value(fixed_sql));
		} else {
			auto line_number = ExtractLineNumber(bind_data.error_message);
			AppendSystemPrompt(options, BuildPromptFixLineSystemPrompt(bind_data.schema_context));
			auto prompt = BuildPromptFixLinePrompt(bind_data.sql, bind_data.error_message, line_number);
			auto replacement = StripMarkdownSqlFence(duckdb_ai::Complete(prompt, options).text);
			std::string error;
			if (!ValidateReadOnlySql(ReplaceSqlLine(bind_data.sql, line_number, TrimAscii(replacement)), error)) {
				throw InvalidInputException("AI corrected SQL line does not produce a single read-only SELECT "
				                            "statement: %s",
				                            error);
			}
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

void AiIsReadOnlySqlFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &sql_vector = args.data[0];
	if (args.ColumnCount() < 2) {
		UnaryExecutor::Execute<string_t, bool>(sql_vector, result, args.size(), [&](string_t sql) {
			std::string error;
			return ValidateReadOnlySql(sql.GetString(), error);
		});
		return;
	}
	BinaryExecutor::Execute<string_t, bool, bool>(
	    sql_vector, args.data[1], result, args.size(), [&](string_t sql, bool check_binding) {
		    auto sql_string = sql.GetString();
		    std::string error;
		    if (!ValidateReadOnlySql(sql_string, error)) {
			    return false;
		    }
		    return !check_binding || VerifySqlBinds(state.GetContext(), sql_string, error);
	    });
}

void AiValidateReadOnlySqlFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &sql_vector = args.data[0];
	if (args.ColumnCount() < 2) {
		UnaryExecutor::Execute<string_t, string_t>(sql_vector, result, args.size(), [&](string_t sql) {
			auto sql_string = sql.GetString();
			std::string error;
			if (!ValidateReadOnlySql(sql_string, error)) {
				throw InvalidInputException("AI generated SQL is not a single read-only SELECT statement: %s", error);
			}
			return StringVector::AddString(result, sql_string);
		});
		return;
	}
	BinaryExecutor::Execute<string_t, bool, string_t>(
	    sql_vector, args.data[1], result, args.size(), [&](string_t sql, bool check_binding) {
		    auto sql_string = sql.GetString();
		    std::string error;
		    if (!ValidateReadOnlySql(sql_string, error)) {
			    throw InvalidInputException("AI generated SQL is not a single read-only SELECT statement: %s", error);
		    }
		    if (check_binding && !VerifySqlBinds(state.GetContext(), sql_string, error)) {
			    throw InvalidInputException("AI generated SQL does not bind: %s", error);
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
	AddTableColumns(return_types, names,
	                {{"name", LogicalType::VARCHAR},
	                 {"provider", LogicalType::VARCHAR},
	                 {"model", LogicalType::VARCHAR},
	                 {"base_url", LogicalType::VARCHAR},
	                 {"scope", LogicalType::VARCHAR},
	                 {"storage", LogicalType::VARCHAR},
	                 {"persistent", LogicalType::BOOLEAN},
	                 {"has_api_key", LogicalType::BOOLEAN}});
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

bool ExternalModelHasCapability(const std::string &capabilities, const std::string &expected) {
	for (idx_t start = 0; start <= capabilities.size();) {
		auto separator = capabilities.find(',', start);
		auto capability = TrimAscii(
		    capabilities.substr(start, separator == std::string::npos ? std::string::npos : separator - start));
		if (LowerAscii(capability) == expected) {
			return true;
		}
		if (separator == std::string::npos) {
			break;
		}
		start = separator + 1;
	}
	return false;
}

unique_ptr<FunctionData> AiModelsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                      vector<string> &names) {
	AddTableColumns(return_types, names,
	                {{"name", LogicalType::VARCHAR},
	                 {"provider", LogicalType::VARCHAR},
	                 {"model", LogicalType::VARCHAR},
	                 {"location", LogicalType::VARCHAR},
	                 {"credential", LogicalType::VARCHAR},
	                 {"model_type", LogicalType::VARCHAR},
	                 {"capabilities", LogicalType::VARCHAR},
	                 {"options", LogicalType::VARCHAR},
	                 {"max_batch_inputs", LogicalType::BIGINT},
	                 {"max_batch_tokens", LogicalType::BIGINT},
	                 {"max_request_bytes", LogicalType::BIGINT},
	                 {"context_tokens", LogicalType::BIGINT},
	                 {"embedding_dimensions", LogicalType::BIGINT},
	                 {"native_batch_jobs", LogicalType::BOOLEAN},
	                 {"input_token_price_per_million", LogicalType::DOUBLE},
	                 {"output_token_price_per_million", LogicalType::DOUBLE},
	                 {"storage", LogicalType::VARCHAR},
	                 {"persistent", LogicalType::BOOLEAN},
	                 {"validation_status", LogicalType::VARCHAR}});
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> AiModelsInit(ClientContext &context, TableFunctionInitInput &) {
	auto result = make_uniq<AiModelsScanData>();
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	result->models = SecretManager::Get(context).AllSecrets(transaction);
	return std::move(result);
}

void AiModelsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<AiModelsScanData>();
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto &secret_manager = SecretManager::Get(context);
	idx_t count = 0;
	while (data.offset < data.models.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.models[data.offset++];
		if (!entry.secret || entry.secret->GetType() != "duckdb_ai_model") {
			continue;
		}
		auto secret = dynamic_cast<const KeyValueSecret *>(entry.secret.get());
		if (!secret) {
			continue;
		}
		std::string provider;
		std::string model;
		std::string location;
		std::string credential;
		std::string model_type;
		std::string capabilities;
		std::string options;
		TryReadSecretString(*secret, "provider", provider);
		TryReadSecretString(*secret, "model", model);
		TryReadSecretString(*secret, "location", location);
		TryReadSecretString(*secret, "credential", credential);
		TryReadSecretString(*secret, "model_type", model_type);
		TryReadSecretString(*secret, "capabilities", capabilities);
		TryReadSecretString(*secret, "options", options);
		std::string validation_status = "valid";
		duckdb_ai::ProviderCapabilities runtime_capabilities;
		bool has_runtime_capabilities = false;
		duckdb_ai::CompletionOptions inspected_options;
		try {
			inspected_options.provider = provider;
			inspected_options.model = model;
			inspected_options.base_url = location;
			inspected_options.model_options = options;
			duckdb_ai::ApplyModelProfileOptions(inspected_options);
			auto embedding_model = model_type == "embedding" || ExternalModelHasCapability(capabilities, "embedding");
			runtime_capabilities = duckdb_ai::GetProviderCapabilities(inspected_options, embedding_model);
			has_runtime_capabilities = true;
			if (!credential.empty()) {
				auto credential_entry = secret_manager.GetSecretByName(transaction, credential);
				if (!credential_entry || !credential_entry->secret ||
				    credential_entry->secret->GetType() != "duckdb_ai") {
					validation_status = "missing_credential";
				} else if (auto credential_secret =
				               dynamic_cast<const KeyValueSecret *>(credential_entry->secret.get())) {
					std::string credential_provider;
					TryReadSecretString(*credential_secret, "provider", credential_provider);
					if (!credential_provider.empty() && duckdb_ai::NormalizeProviderName(credential_provider) !=
					                                        duckdb_ai::NormalizeProviderName(provider)) {
						validation_status = "credential_provider_mismatch";
					}
				}
			}
		} catch (std::exception &ex) {
			validation_status = "invalid: " + std::string(ex.what());
		}
		double input_price = -1;
		double output_price = -1;
		if (inspected_options.has_input_token_price_per_million) {
			input_price = inspected_options.input_token_price_per_million;
		}
		if (inspected_options.has_output_token_price_per_million) {
			output_price = inspected_options.output_token_price_per_million;
		}
		if (input_price < 0 || output_price < 0) {
			auto operation = model_type == "embedding" || ExternalModelHasCapability(capabilities, "embedding")
			                     ? std::string("embedding")
			                     : std::string("completion");
			for (auto &price : duckdb_ai::ModelPrices()) {
				if (duckdb_ai::NormalizeProviderName(price.provider) == duckdb_ai::NormalizeProviderName(provider) &&
				    LowerAscii(price.model) == LowerAscii(model) && price.operation == operation) {
					if (input_price < 0) {
						input_price = price.input_token_price_per_million;
					}
					if (output_price < 0) {
						output_price = price.output_token_price_per_million;
					}
					break;
				}
			}
		}
		auto name = entry.secret->GetName();
		auto prefix = ExternalModelSecretName("");
		if (StartsWith(name, prefix)) {
			name = name.substr(prefix.size());
		}
		idx_t col = 0;
		output.SetValue(col++, count, Value(name));
		output.SetValue(col++, count, Value(provider));
		output.SetValue(col++, count, Value(model));
		output.SetValue(col++, count, location.empty() ? Value() : Value(location));
		output.SetValue(col++, count, credential.empty() ? Value() : Value(credential));
		output.SetValue(col++, count, model_type.empty() ? Value() : Value(model_type));
		output.SetValue(col++, count, capabilities.empty() ? Value() : Value(capabilities));
		output.SetValue(col++, count, options.empty() ? Value() : Value(options));
		output.SetValue(col++, count,
		                has_runtime_capabilities ? Value::BIGINT(runtime_capabilities.max_batch_inputs) : Value());
		output.SetValue(col++, count,
		                has_runtime_capabilities ? Value::BIGINT(runtime_capabilities.max_batch_tokens) : Value());
		output.SetValue(col++, count,
		                has_runtime_capabilities ? Value::BIGINT(runtime_capabilities.max_request_bytes) : Value());
		output.SetValue(col++, count,
		                has_runtime_capabilities && runtime_capabilities.context_tokens >= 0
		                    ? Value::BIGINT(runtime_capabilities.context_tokens)
		                    : Value());
		output.SetValue(col++, count,
		                has_runtime_capabilities && runtime_capabilities.embedding_dimensions >= 0
		                    ? Value::BIGINT(runtime_capabilities.embedding_dimensions)
		                    : Value());
		output.SetValue(col++, count,
		                has_runtime_capabilities ? Value::BOOLEAN(runtime_capabilities.native_batch_jobs) : Value());
		output.SetValue(col++, count, input_price >= 0 ? Value::DOUBLE(input_price) : Value());
		output.SetValue(col++, count, output_price >= 0 ? Value::DOUBLE(output_price) : Value());
		output.SetValue(col++, count, Value(entry.storage_mode));
		output.SetValue(col++, count, Value::BOOLEAN(entry.persist_type == SecretPersistType::PERSISTENT));
		output.SetValue(col++, count, Value(validation_status));
		count++;
	}
	output.SetCardinality(count);
}

std::string ExternalModelInputString(const Value &input) {
	if (input.IsNull()) {
		return "";
	}
	auto value = input;
	return StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
}

unique_ptr<FunctionData> ExternalModelBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 9) {
		throw BinderException("CREATE EXTERNAL MODEL internal plan expected nine arguments");
	}
	auto bind_data = make_uniq<ExternalModelBindData>();
	bind_data->name = ExternalModelInputString(input.inputs[0]);
	bind_data->provider = duckdb_ai::NormalizeProviderName(ExternalModelInputString(input.inputs[1]));
	bind_data->model = ExternalModelInputString(input.inputs[2]);
	bind_data->location = ExternalModelInputString(input.inputs[3]);
	bind_data->credential = ExternalModelInputString(input.inputs[4]);
	bind_data->model_type = LowerAscii(ExternalModelInputString(input.inputs[5]));
	bind_data->capabilities = ExternalModelInputString(input.inputs[6]);
	bind_data->options = ExternalModelInputString(input.inputs[7]);
	auto replace_value = input.inputs[8].DefaultCastAs(LogicalType::BOOLEAN);
	bind_data->replace = BooleanValue::Get(replace_value);
	if (bind_data->name.empty() || bind_data->provider.empty() || bind_data->model.empty()) {
		throw BinderException("CREATE EXTERNAL MODEL requires a name, provider, and model");
	}
	if (!bind_data->model_type.empty() && bind_data->model_type != "completion" &&
	    bind_data->model_type != "embedding") {
		throw BinderException("CREATE EXTERNAL MODEL model_type must be completion or embedding");
	}
	if (!bind_data->options.empty()) {
		std::string error;
		if (!duckdb_ai::ValidateJsonDocument(bind_data->options, error)) {
			throw BinderException("CREATE EXTERNAL MODEL options must be valid JSON: %s", error);
		}
	}
	AddTableColumns(return_types, names, {{"model_name", LogicalType::VARCHAR}, {"created", LogicalType::BOOLEAN}});
	return std::move(bind_data);
}

void ExternalModelFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<AiClearUsageScanData>();
	if (state.emitted) {
		return;
	}
	auto &bind_data = data_p.bind_data->Cast<ExternalModelBindData>();
	CreateSecretInput input;
	input.type = "duckdb_ai_model";
	input.provider = "config";
	input.name = ExternalModelSecretName(bind_data.name);
	input.on_conflict = bind_data.replace ? OnCreateConflict::REPLACE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	input.persist_type = SecretPersistType::DEFAULT;
	input.options["ai_provider"] = Value(bind_data.provider);
	input.options["model"] = Value(bind_data.model);
	if (!bind_data.location.empty()) {
		input.options["location"] = Value(bind_data.location);
	}
	if (!bind_data.credential.empty()) {
		input.options["credential"] = Value(bind_data.credential);
	}
	if (!bind_data.model_type.empty()) {
		input.options["model_type"] = Value(bind_data.model_type);
	}
	if (!bind_data.capabilities.empty()) {
		input.options["capabilities"] = Value(bind_data.capabilities);
	}
	if (!bind_data.options.empty()) {
		input.options["options"] = Value(bind_data.options);
	}
	SecretManager::Get(context).CreateSecret(context, input);
	output.SetValue(0, 0, Value(bind_data.name));
	output.SetValue(1, 0, Value::BOOLEAN(true));
	output.SetCardinality(1);
	state.emitted = true;
}

TableFunction ExternalModelTableFunction() {
	auto init = [](ClientContext &, TableFunctionInitInput &) -> unique_ptr<GlobalTableFunctionState> {
		return make_uniq<AiClearUsageScanData>();
	};
	return TableFunction("duckdb_ai_create_external_model",
	                     {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                      LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                      LogicalType::BOOLEAN},
	                     ExternalModelFunction, ExternalModelBind, init);
}

struct ExternalModelDefinition {
	std::string name;
	std::string provider;
	std::string model;
	std::string location;
	std::string credential;
	std::string model_type;
	std::string capabilities;
	std::string options;
};

ExternalModelDefinition GetExternalModel(ClientContext &context, const std::string &name) {
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto entry = SecretManager::Get(context).GetSecretByName(transaction, ExternalModelSecretName(name));
	if (!entry || !entry->secret || entry->secret->GetType() != "duckdb_ai_model") {
		throw InvalidInputException("External model \"%s\" was not found", name);
	}
	auto secret = dynamic_cast<const KeyValueSecret *>(entry->secret.get());
	if (!secret) {
		throw InternalException("External model \"%s\" has an invalid catalog representation", name);
	}
	ExternalModelDefinition result;
	result.name = name;
	TryReadSecretString(*secret, "provider", result.provider);
	TryReadSecretString(*secret, "model", result.model);
	TryReadSecretString(*secret, "location", result.location);
	TryReadSecretString(*secret, "credential", result.credential);
	TryReadSecretString(*secret, "model_type", result.model_type);
	TryReadSecretString(*secret, "capabilities", result.capabilities);
	TryReadSecretString(*secret, "options", result.options);
	return result;
}

unique_ptr<FunctionData> ControlPlaneBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names,
                                          ControlPlaneOperation operation) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("AI control-plane functions require one non-NULL profile or operation id");
	}
	auto bind_data = make_uniq<ControlPlaneBindData>(operation);
	bind_data->argument = ExternalModelInputString(input.inputs[0]);
	if (bind_data->argument.empty()) {
		throw BinderException("AI control-plane profile or operation id must not be empty");
	}
	ApplySettings(context, bind_data->options);
	duckdb_ai::AttachProviderRuntimeState(bind_data->options, context);
	if (operation == ControlPlaneOperation::PROVISION) {
		for (auto &parameter : input.named_parameters) {
			auto name = LowerAscii(parameter.first);
			auto value = parameter.second;
			if (name == "dry_run") {
				value = value.DefaultCastAs(LogicalType::BOOLEAN);
				bind_data->dry_run = BooleanValue::Get(value);
			} else if (name == "max_hourly_cost_usd") {
				value = value.DefaultCastAs(LogicalType::DOUBLE);
				bind_data->max_hourly_cost_usd = DoubleValue::Get(value);
				bind_data->has_max_hourly_cost = true;
			} else {
				throw BinderException("Unsupported ai_provision_endpoint option \"%s\"", name);
			}
		}
		if (bind_data->has_max_hourly_cost &&
		    (!std::isfinite(bind_data->max_hourly_cost_usd) || bind_data->max_hourly_cost_usd <= 0)) {
			throw BinderException("ai_provision_endpoint max_hourly_cost_usd must be finite and greater than 0");
		}
		if (!bind_data->dry_run && !bind_data->has_max_hourly_cost) {
			throw BinderException("ai_provision_endpoint requires max_hourly_cost_usd > 0 when dry_run is false");
		}
	}
	AddTableColumns(return_types, names,
	                {{"operation_id", LogicalType::VARCHAR},
	                 {"status", LogicalType::VARCHAR},
	                 {"endpoint_url", LogicalType::VARCHAR},
	                 {"estimated_hourly_cost_usd", LogicalType::DOUBLE},
	                 {"action_required", LogicalType::VARCHAR},
	                 {"response", LogicalType::VARCHAR}});
	return std::move(bind_data);
}

unique_ptr<FunctionData> ProvisionEndpointBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	return ControlPlaneBind(context, input, return_types, names, ControlPlaneOperation::PROVISION);
}

unique_ptr<FunctionData> EndpointStatusBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	return ControlPlaneBind(context, input, return_types, names, ControlPlaneOperation::STATUS);
}

unique_ptr<FunctionData> DeprovisionEndpointBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	return ControlPlaneBind(context, input, return_types, names, ControlPlaneOperation::DEPROVISION);
}

using SqlYyjsonDoc = std::unique_ptr<duckdb_yyjson::yyjson_doc, decltype(&duckdb_yyjson::yyjson_doc_free)>;

SqlYyjsonDoc ParseSqlJson(const std::string &input) {
	duckdb_yyjson::yyjson_read_err error;
	auto doc = duckdb_yyjson::yyjson_read_opts(const_cast<char *>(input.data()), input.size(),
	                                           duckdb_yyjson::YYJSON_READ_NOFLAG, nullptr, &error);
	return SqlYyjsonDoc(doc, duckdb_yyjson::yyjson_doc_free);
}

std::string SqlJsonString(duckdb_yyjson::yyjson_val *root, const char *name) {
	if (!root || !duckdb_yyjson::yyjson_is_obj(root)) {
		return "";
	}
	auto value = duckdb_yyjson::yyjson_obj_get(root, name);
	if (!value || !duckdb_yyjson::yyjson_is_str(value)) {
		return "";
	}
	return std::string(duckdb_yyjson::yyjson_get_str(value), duckdb_yyjson::yyjson_get_len(value));
}

bool SqlJsonDouble(duckdb_yyjson::yyjson_val *root, const char *name, double &result) {
	if (!root || !duckdb_yyjson::yyjson_is_obj(root)) {
		return false;
	}
	auto value = duckdb_yyjson::yyjson_obj_get(root, name);
	if (!value || !duckdb_yyjson::yyjson_is_num(value)) {
		return false;
	}
	result = duckdb_yyjson::yyjson_get_num(value);
	return std::isfinite(result);
}

void ControlPlaneFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<AiClearUsageScanData>();
	if (state.emitted) {
		return;
	}
	auto &bind_data = data_p.bind_data->Cast<ControlPlaneBindData>();
	std::string response;
	std::string operation_id;
	std::string status;
	std::string endpoint_url;
	std::string action_required;
	double estimated_cost = 0;
	bool has_estimated_cost = false;
	if (bind_data.operation == ControlPlaneOperation::PROVISION && bind_data.dry_run) {
		auto model = GetExternalModel(context, bind_data.argument);
		status = "planned";
		endpoint_url = model.location;
		action_required = "rerun with dry_run := false and an explicit max_hourly_cost_usd ceiling";
		response = "{\"profile\":\"" + JsonEscapeSqlText(model.name) + "\",\"provider\":\"" +
		           JsonEscapeSqlText(model.provider) + "\",\"model\":\"" + JsonEscapeSqlText(model.model) +
		           "\",\"dry_run\":true}";
	} else {
		std::string path;
		std::string payload;
		if (bind_data.operation == ControlPlaneOperation::PROVISION) {
			auto model = GetExternalModel(context, bind_data.argument);
			path = "/v1/endpoints/provision";
			payload = "{\"profile\":\"" + JsonEscapeSqlText(model.name) + "\",\"provider\":\"" +
			          JsonEscapeSqlText(model.provider) + "\",\"model\":\"" + JsonEscapeSqlText(model.model) +
			          "\",\"location\":\"" + JsonEscapeSqlText(model.location) + "\",\"credential\":\"" +
			          JsonEscapeSqlText(model.credential) +
			          "\",\"max_hourly_cost_usd\":" + std::to_string(bind_data.max_hourly_cost_usd) + "}";
		} else if (bind_data.operation == ControlPlaneOperation::STATUS) {
			path = "/v1/endpoints/status";
			payload = "{\"operation_id\":\"" + JsonEscapeSqlText(bind_data.argument) + "\"}";
		} else {
			auto model = GetExternalModel(context, bind_data.argument);
			path = "/v1/endpoints/deprovision";
			payload = "{\"profile\":\"" + JsonEscapeSqlText(model.name) + "\"}";
		}
		response = duckdb_ai::ControlPlaneRequest(path, payload, bind_data.options);
		auto doc = ParseSqlJson(response);
		auto root = doc ? duckdb_yyjson::yyjson_doc_get_root(doc.get()) : nullptr;
		operation_id = SqlJsonString(root, "operation_id");
		status = SqlJsonString(root, "status");
		endpoint_url = SqlJsonString(root, "endpoint_url");
		action_required = SqlJsonString(root, "action_required");
		has_estimated_cost = SqlJsonDouble(root, "estimated_hourly_cost_usd", estimated_cost);
	}
	idx_t col = 0;
	output.SetValue(col++, 0, operation_id.empty() ? Value() : Value(operation_id));
	output.SetValue(col++, 0, status.empty() ? Value() : Value(status));
	output.SetValue(col++, 0, endpoint_url.empty() ? Value() : Value(endpoint_url));
	output.SetValue(col++, 0, has_estimated_cost ? Value::DOUBLE(estimated_cost) : Value());
	output.SetValue(col++, 0, action_required.empty() ? Value() : Value(action_required));
	output.SetValue(col++, 0, Value(response));
	output.SetCardinality(1);
	state.emitted = true;
}

std::string Base64Encode(const std::string &input) {
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string output;
	output.reserve(((input.size() + 2) / 3) * 4);
	for (idx_t i = 0; i < input.size(); i += 3) {
		auto a = static_cast<unsigned char>(input[i]);
		auto b = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
		auto c = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
		auto packed = (static_cast<uint32_t>(a) << 16) | (static_cast<uint32_t>(b) << 8) | c;
		output.push_back(alphabet[(packed >> 18) & 0x3f]);
		output.push_back(alphabet[(packed >> 12) & 0x3f]);
		output.push_back(i + 1 < input.size() ? alphabet[(packed >> 6) & 0x3f] : '=');
		output.push_back(i + 2 < input.size() ? alphabet[packed & 0x3f] : '=');
	}
	return output;
}

std::string SqlJsonValueText(duckdb_yyjson::yyjson_val *root, const char *name) {
	if (!root || !duckdb_yyjson::yyjson_is_obj(root)) {
		return "";
	}
	auto value = duckdb_yyjson::yyjson_obj_get(root, name);
	if (!value || duckdb_yyjson::yyjson_is_null(value)) {
		return "";
	}
	if (duckdb_yyjson::yyjson_is_str(value)) {
		return std::string(duckdb_yyjson::yyjson_get_str(value), duckdb_yyjson::yyjson_get_len(value));
	}
	auto serialized = duckdb_yyjson::yyjson_val_write(value, duckdb_yyjson::YYJSON_WRITE_NOFLAG, nullptr);
	if (!serialized) {
		return "";
	}
	std::string result(serialized);
	std::free(serialized);
	return result;
}

int64_t SqlJsonInteger(duckdb_yyjson::yyjson_val *root, const char *name) {
	if (!root || !duckdb_yyjson::yyjson_is_obj(root)) {
		return -1;
	}
	auto value = duckdb_yyjson::yyjson_obj_get(root, name);
	if (!value || !duckdb_yyjson::yyjson_is_int(value)) {
		return -1;
	}
	if (duckdb_yyjson::yyjson_is_uint(value)) {
		auto unsigned_value = duckdb_yyjson::yyjson_get_uint(value);
		if (unsigned_value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			return -1;
		}
		return static_cast<int64_t>(unsigned_value);
	}
	return duckdb_yyjson::yyjson_get_sint(value);
}

unique_ptr<FunctionData> DocumentParseBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 3 || input.inputs[0].IsNull() || input.inputs[1].IsNull() || input.inputs[2].IsNull()) {
		throw BinderException("ai_parse_document requires non-NULL content, mime_type, and parser_profile arguments");
	}
	auto bind_data = make_uniq<DocumentParseBindData>();
	auto content = input.inputs[0].DefaultCastAs(LogicalType::BLOB);
	bind_data->content = StringValue::Get(content);
	bind_data->mime_type = ExternalModelInputString(input.inputs[1]);
	bind_data->parser_profile = ExternalModelInputString(input.inputs[2]);
	ApplySettings(context, bind_data->options);
	StampProviderFunction(bind_data->options, "ai_parse_document");
	duckdb_ai::AttachProviderRuntimeState(bind_data->options, context);
	for (auto &parameter : input.named_parameters) {
		auto name = LowerAscii(parameter.first);
		if (name == "pages") {
			auto value = parameter.second;
			bind_data->pages = value.IsNull() ? "" : StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
		} else if (!ApplyCompletionValueOption(bind_data->options, "ai_parse_document", name, parameter.second, true,
		                                       false)) {
			throw BinderException("Unsupported ai_parse_document option \"%s\"", name);
		}
	}
	AddTableColumns(return_types, names,
	                {{"document_id", LogicalType::VARCHAR},
	                 {"page", LogicalType::BIGINT},
	                 {"element_index", LogicalType::BIGINT},
	                 {"element_type", LogicalType::VARCHAR},
	                 {"text", LogicalType::VARCHAR},
	                 {"markdown", LogicalType::VARCHAR},
	                 {"bbox", LogicalType::VARCHAR},
	                 {"confidence", LogicalType::DOUBLE},
	                 {"metadata", LogicalType::VARCHAR},
	                 {"error", LogicalType::VARCHAR}});
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> DocumentParseInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<DocumentParseScanData>();
}

void LoadDocumentElements(const DocumentParseBindData &bind_data, DocumentParseScanData &state) {
	if (state.loaded) {
		return;
	}
	state.loaded = true;
	try {
		auto payload = "{\"content_base64\":\"" + Base64Encode(bind_data.content) + "\",\"mime_type\":\"" +
		               JsonEscapeSqlText(bind_data.mime_type) + "\",\"parser_profile\":\"" +
		               JsonEscapeSqlText(bind_data.parser_profile) + "\",\"pages\":\"" +
		               JsonEscapeSqlText(bind_data.pages) + "\"}";
		auto response = duckdb_ai::ControlPlaneRequest("/v1/documents/parse", payload, bind_data.options);
		auto doc = ParseSqlJson(response);
		auto root = doc ? duckdb_yyjson::yyjson_doc_get_root(doc.get()) : nullptr;
		auto document_id = SqlJsonString(root, "document_id");
		auto root_error = SqlJsonString(root, "error");
		auto elements =
		    root && duckdb_yyjson::yyjson_is_obj(root) ? duckdb_yyjson::yyjson_obj_get(root, "elements") : nullptr;
		if (!elements || !duckdb_yyjson::yyjson_is_arr(elements)) {
			if (!root_error.empty()) {
				DocumentElement result;
				result.document_id = document_id;
				result.error = root_error;
				state.elements.push_back(std::move(result));
				return;
			}
			throw IOException("AI document parser response must contain an elements array");
		}
		if (elements && duckdb_yyjson::yyjson_is_arr(elements)) {
			duckdb_yyjson::yyjson_val *element;
			size_t index;
			size_t max;
			yyjson_arr_foreach(elements, index, max, element) {
				DocumentElement result;
				result.document_id = document_id;
				if (!duckdb_yyjson::yyjson_is_obj(element)) {
					result.error = "AI document parser element must be an object";
					state.elements.push_back(std::move(result));
					continue;
				}
				result.page = SqlJsonInteger(element, "page");
				result.element_index = SqlJsonInteger(element, "element_index");
				result.element_type = SqlJsonString(element, "element_type");
				result.text = SqlJsonString(element, "text");
				result.markdown = SqlJsonString(element, "markdown");
				result.bbox = SqlJsonValueText(element, "bbox");
				result.metadata = SqlJsonValueText(element, "metadata");
				result.error = SqlJsonString(element, "error");
				result.has_confidence = SqlJsonDouble(element, "confidence", result.confidence);
				state.elements.push_back(std::move(result));
			}
		}
		if (state.elements.empty()) {
			DocumentElement result;
			result.document_id = document_id;
			result.error = root_error.empty() ? "AI document parser returned no elements" : root_error;
			state.elements.push_back(std::move(result));
		}
	} catch (std::exception &ex) {
		if (bind_data.options.fail_on_error) {
			throw;
		}
		DocumentElement result;
		result.error = ex.what();
		state.elements.push_back(std::move(result));
	}
}

void DocumentParseFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<DocumentParseBindData>();
	auto &data = data_p.global_state->Cast<DocumentParseScanData>();
	LoadDocumentElements(bind_data, data);
	idx_t count = 0;
	while (data.offset < data.elements.size() && count < STANDARD_VECTOR_SIZE) {
		auto &element = data.elements[data.offset++];
		idx_t col = 0;
		output.SetValue(col++, count, element.document_id.empty() ? Value() : Value(element.document_id));
		output.SetValue(col++, count, element.page < 0 ? Value() : Value::BIGINT(element.page));
		output.SetValue(col++, count, element.element_index < 0 ? Value() : Value::BIGINT(element.element_index));
		output.SetValue(col++, count, element.element_type.empty() ? Value() : Value(element.element_type));
		output.SetValue(col++, count, element.text.empty() ? Value() : Value(element.text));
		output.SetValue(col++, count, element.markdown.empty() ? Value() : Value(element.markdown));
		output.SetValue(col++, count, element.bbox.empty() ? Value() : Value(element.bbox));
		output.SetValue(col++, count, element.has_confidence ? Value::DOUBLE(element.confidence) : Value());
		output.SetValue(col++, count, element.metadata.empty() ? Value() : Value(element.metadata));
		output.SetValue(col++, count, element.error.empty() ? Value() : Value(element.error));
		count++;
	}
	output.SetCardinality(count);
}

struct ParsedClassifierArtifact {
	bool usable = false;
	double accuracy = 0;
	double confidence_margin = 0.05;
	std::vector<std::string> labels;
	std::vector<std::vector<double>> centroids;
	duckdb_ai::CompletionOptions embedding_options;
};

struct OptimizedClassifierRow {
	idx_t row = 0;
	std::string input;
	ParsedClassifierArtifact artifact;
	std::vector<double> embedding;
	std::string local_error;
	double best_score = 0;
	double score_margin = 0;
	idx_t best_label = DConstants::INVALID_INDEX;
};

bool RequiredJsonBoolean(duckdb_yyjson::yyjson_val *root, const char *name) {
	auto value = root && duckdb_yyjson::yyjson_is_obj(root) ? duckdb_yyjson::yyjson_obj_get(root, name) : nullptr;
	if (!value || !duckdb_yyjson::yyjson_is_bool(value)) {
		throw InvalidInputException("classifier artifact field \"%s\" must be a boolean", name);
	}
	return duckdb_yyjson::yyjson_get_bool(value);
}

double RequiredJsonNumber(duckdb_yyjson::yyjson_val *root, const char *name) {
	double result;
	if (!SqlJsonDouble(root, name, result)) {
		throw InvalidInputException("classifier artifact field \"%s\" must be a finite number", name);
	}
	return result;
}

ParsedClassifierArtifact ParseClassifierArtifact(ClientContext &context, const std::string &input) {
	auto doc = ParseSqlJson(input);
	auto root = doc ? duckdb_yyjson::yyjson_doc_get_root(doc.get()) : nullptr;
	if (!root || !duckdb_yyjson::yyjson_is_obj(root) || SqlJsonInteger(root, "version") != 1 ||
	    SqlJsonString(root, "optimization") != "minimize_cost") {
		throw InvalidInputException("invalid ai_build_classifier artifact");
	}
	ParsedClassifierArtifact result;
	result.usable = RequiredJsonBoolean(root, "usable");
	result.accuracy = RequiredJsonNumber(root, "accuracy");
	result.confidence_margin = RequiredJsonNumber(root, "confidence_margin");
	if (result.confidence_margin < 0 || result.confidence_margin > 1) {
		throw InvalidInputException("classifier artifact confidence_margin must be between 0 and 1");
	}
	auto labels = duckdb_yyjson::yyjson_obj_get(root, "labels");
	if (!labels || !duckdb_yyjson::yyjson_is_arr(labels)) {
		throw InvalidInputException("classifier artifact labels must be an array");
	}
	duckdb_yyjson::yyjson_val *entry;
	size_t index;
	size_t max;
	yyjson_arr_foreach(labels, index, max, entry) {
		if (!duckdb_yyjson::yyjson_is_str(entry)) {
			throw InvalidInputException("classifier artifact labels must contain only strings");
		}
		result.labels.emplace_back(duckdb_yyjson::yyjson_get_str(entry), duckdb_yyjson::yyjson_get_len(entry));
	}
	std::set<std::string> unique_labels;
	for (auto &label : result.labels) {
		if (label.empty() || !unique_labels.insert(LowerAscii(label)).second) {
			throw InvalidInputException("classifier artifact labels must be non-empty and unique");
		}
	}
	auto centroids = duckdb_yyjson::yyjson_obj_get(root, "centroids");
	if (!centroids || !duckdb_yyjson::yyjson_is_arr(centroids)) {
		throw InvalidInputException("classifier artifact centroids must be an array");
	}
	yyjson_arr_foreach(centroids, index, max, entry) {
		if (!duckdb_yyjson::yyjson_is_arr(entry)) {
			throw InvalidInputException("classifier artifact centroids must be numeric arrays");
		}
		std::vector<double> centroid;
		duckdb_yyjson::yyjson_val *coordinate;
		size_t coordinate_index;
		size_t coordinate_max;
		yyjson_arr_foreach(entry, coordinate_index, coordinate_max, coordinate) {
			if (!duckdb_yyjson::yyjson_is_num(coordinate)) {
				throw InvalidInputException("classifier artifact centroids must contain only numbers");
			}
			auto value = duckdb_yyjson::yyjson_get_num(coordinate);
			if (!std::isfinite(value)) {
				throw InvalidInputException("classifier artifact centroid values must be finite");
			}
			centroid.push_back(value);
		}
		result.centroids.push_back(std::move(centroid));
	}
	if (result.labels.size() < 2 || result.labels.size() != result.centroids.size() || result.centroids[0].empty()) {
		throw InvalidInputException("classifier artifact must contain matching labels and non-empty centroids");
	}
	for (auto &centroid : result.centroids) {
		if (centroid.size() != result.centroids[0].size()) {
			throw InvalidInputException("classifier artifact centroid dimensions are inconsistent");
		}
	}
	auto embedding = duckdb_yyjson::yyjson_obj_get(root, "embedding");
	if (!embedding || !duckdb_yyjson::yyjson_is_obj(embedding)) {
		throw InvalidInputException("classifier artifact embedding configuration is missing");
	}
	ApplySettings(context, result.embedding_options, AiModelSettingKind::EMBEDDING);
	result.embedding_options.provider = SqlJsonString(embedding, "provider");
	result.embedding_options.model = SqlJsonString(embedding, "model");
	result.embedding_options.secret_name = SqlJsonString(embedding, "profile");
	result.embedding_options.base_url = SqlJsonString(embedding, "base_url");
	result.embedding_options.model_options = SqlJsonString(embedding, "options");
	if (result.embedding_options.provider.empty() || result.embedding_options.model.empty()) {
		throw InvalidInputException("classifier artifact embedding provider and model are required");
	}
	if (result.accuracy < 0 || result.accuracy > 1) {
		throw InvalidInputException("classifier artifact accuracy must be between 0 and 1");
	}
	StampProviderFunction(result.embedding_options, "ai_classify_optimized_embed");
	duckdb_ai::AttachProviderRuntimeState(result.embedding_options, context);
	return result;
}

LogicalType AiOptimizedClassifierReturnType() {
	child_list_t<LogicalType> children;
	children.emplace_back("value", LogicalType::VARCHAR);
	children.emplace_back("used_fallback", LogicalType::BOOLEAN);
	children.emplace_back("confidence", LogicalType::DOUBLE);
	children.emplace_back("error", LogicalType::VARCHAR);
	children.emplace_back("metadata", LogicalType::VARCHAR);
	return LogicalType::STRUCT(std::move(children));
}

Value AiOptimizedClassifierResultValue(const LogicalType &result_type, const std::string &value, bool used_fallback,
                                       double confidence, bool has_confidence, const std::string &error,
                                       const std::string &metadata) {
	vector<Value> children;
	children.push_back(value.empty() ? Value(LogicalType::VARCHAR) : Value(value));
	children.push_back(Value::BOOLEAN(used_fallback));
	children.push_back(has_confidence ? Value::DOUBLE(confidence) : Value(LogicalType::DOUBLE));
	children.push_back(error.empty() ? Value(LogicalType::VARCHAR) : Value(error));
	children.push_back(metadata.empty() ? Value(LogicalType::VARCHAR) : Value(metadata));
	return Value::STRUCT(result_type, std::move(children));
}

unique_ptr<FunctionData> AiOptimizedClassifierBind(ClientContext &context, ScalarFunction &bound_function,
                                                   vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() < 2) {
		throw BinderException("ai_classify_optimized requires input and classifier artifact arguments");
	}
	auto bind_data = make_uniq<OptimizedClassifierBindData>();
	ApplySettings(context, bind_data->fallback_options, AiModelSettingKind::TASK);
	StampProviderFunction(bind_data->fallback_options, "ai_classify_optimized_fallback");
	duckdb_ai::AttachProviderRuntimeState(bind_data->fallback_options, context);
	for (idx_t i = 2; i < arguments.size(); i++) {
		auto alias = LowerAscii(arguments[i]->GetAlias());
		if (alias.empty()) {
			throw BinderException("ai_classify_optimized options must use named arguments");
		}
		if (alias == "fallback_profile") {
			bind_data->fallback_options.secret_name = EvaluateConstantString(context, *arguments[i], alias);
			bound_function.arguments.emplace_back(LogicalType::VARCHAR);
			continue;
		}
		LogicalType option_type;
		if (!TryGetOptionType(alias, option_type)) {
			throw BinderException("Unsupported ai_classify_optimized option \"%s\"", alias);
		}
		ApplyNamedOption(context, bind_data->fallback_options, *arguments[i], alias);
		bound_function.arguments.emplace_back(option_type);
	}
	bind_data->fallback_options.on_error = "capture";
	bind_data->fallback_options.fail_on_error = false;
	ApplyAiProviderSecret(context, bind_data->fallback_options);
	bound_function.SetReturnType(AiOptimizedClassifierReturnType());
	return std::move(bind_data);
}

std::string OptimizedClassifierMetadata(const OptimizedClassifierRow &row, const std::string &path,
                                        const std::string &reason, const std::string &provider = "",
                                        const std::string &model = "") {
	std::ostringstream output;
	output << std::setprecision(17) << "{\"path\":\"" << JsonEscapeSqlText(path)
	       << "\",\"artifact_accuracy\":" << row.artifact.accuracy;
	if (!reason.empty()) {
		output << ",\"reason\":\"" << JsonEscapeSqlText(reason) << '"';
	}
	if (row.best_label != DConstants::INVALID_INDEX) {
		output << ",\"cosine_score\":" << row.best_score << ",\"centroid_margin\":" << row.score_margin;
	}
	if (!provider.empty()) {
		output << ",\"provider\":\"" << JsonEscapeSqlText(provider) << '"';
	}
	if (!model.empty()) {
		output << ",\"model\":\"" << JsonEscapeSqlText(model) << '"';
	}
	output << '}';
	return output.str();
}

void AiOptimizedClassifierFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<OptimizedClassifierBindData>();
	auto result_type = AiOptimizedClassifierReturnType();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_validity = FlatVector::Validity(result);
	StringVectorReader input_reader(args, 0);
	StringVectorReader artifact_reader(args, 1);
	std::vector<OptimizedClassifierRow> rows;
	std::vector<SecretResolutionCacheEntry> embedding_secret_cache;
	for (idx_t row_index = 0; row_index < args.size(); row_index++) {
		std::string input;
		std::string artifact;
		if (!input_reader.Read(row_index, input) || !artifact_reader.Read(row_index, artifact)) {
			result_validity.SetInvalid(row_index);
			continue;
		}
		try {
			OptimizedClassifierRow row;
			row.row = row_index;
			row.input = std::move(input);
			row.artifact = ParseClassifierArtifact(state.GetContext(), artifact);
			row.artifact.embedding_options.query_id = bind_data.fallback_options.query_id + ":embed";
			if (!row.artifact.usable) {
				row.local_error = "artifact quality threshold was not met";
			} else {
				ApplyAiProviderSecretCached(state.GetContext(), row.artifact.embedding_options, embedding_secret_cache);
			}
			rows.push_back(std::move(row));
		} catch (std::exception &ex) {
			auto metadata = "{\"path\":\"error\",\"reason\":\"invalid classifier artifact\"}";
			result.SetValue(row_index,
			                AiOptimizedClassifierResultValue(result_type, "", false, 0, false, ex.what(), metadata));
		}
	}

	std::vector<bool> grouped(rows.size(), false);
	for (idx_t group_start = 0; group_start < rows.size(); group_start++) {
		if (grouped[group_start] || !rows[group_start].local_error.empty()) {
			continue;
		}
		std::vector<idx_t> group_rows;
		std::vector<std::string> unique_inputs;
		std::unordered_map<std::string, idx_t> input_indexes;
		for (idx_t row = group_start; row < rows.size(); row++) {
			if (grouped[row] || !rows[row].local_error.empty() ||
			    !CompletionOptionsEqual(rows[group_start].artifact.embedding_options,
			                            rows[row].artifact.embedding_options)) {
				continue;
			}
			grouped[row] = true;
			group_rows.push_back(row);
			if (input_indexes.find(rows[row].input) == input_indexes.end()) {
				input_indexes[rows[row].input] = unique_inputs.size();
				unique_inputs.push_back(rows[row].input);
			}
		}
		try {
			auto embeddings = duckdb_ai::EmbedMany(unique_inputs, rows[group_start].artifact.embedding_options);
			if (embeddings.size() != unique_inputs.size()) {
				throw InvalidInputException("embedding provider returned the wrong number of classifier embeddings");
			}
			for (auto row : group_rows) {
				rows[row].embedding = embeddings[input_indexes[rows[row].input]].values;
			}
		} catch (std::exception &ex) {
			for (auto row : group_rows) {
				rows[row].local_error = std::string("local embedding failed: ") + ex.what();
			}
		}
	}

	std::vector<ProviderStringJob> fallback_jobs;
	std::unordered_map<idx_t, idx_t> fallback_row_indexes;
	std::vector<SecretResolutionCacheEntry> fallback_secret_cache;
	for (idx_t row_index = 0; row_index < rows.size(); row_index++) {
		auto &row = rows[row_index];
		if (row.local_error.empty()) {
			try {
				double best = -std::numeric_limits<double>::infinity();
				double second = -std::numeric_limits<double>::infinity();
				for (idx_t label = 0; label < row.artifact.centroids.size(); label++) {
					auto score = CosineSimilarity(row.embedding, row.artifact.centroids[label]);
					if (score > best) {
						second = best;
						best = score;
						row.best_label = label;
					} else if (score > second) {
						second = score;
					}
				}
				row.best_score = best;
				row.score_margin = best - second;
				if (row.score_margin >= row.artifact.confidence_margin) {
					auto metadata = OptimizedClassifierMetadata(row, "local", "");
					result.SetValue(row.row,
					                AiOptimizedClassifierResultValue(result_type, row.artifact.labels[row.best_label],
					                                                 false, row.best_score, true, "", metadata));
					continue;
				}
				row.local_error = "centroid margin below confidence threshold";
			} catch (std::exception &ex) {
				row.local_error = std::string("local classification failed: ") + ex.what();
			}
		}
		try {
			auto options = bind_data.fallback_options;
			ApplyAiProviderSecretCached(state.GetContext(), options, fallback_secret_cache);
			ProviderStringJob job;
			job.row = row.row;
			job.input = row.input;
			job.parameter = JoinClassifierLabels(row.artifact.labels);
			job.options = std::move(options);
			job.fail_on_error = false;
			fallback_row_indexes[job.row] = row_index;
			fallback_jobs.push_back(std::move(job));
		} catch (std::exception &ex) {
			auto metadata = OptimizedClassifierMetadata(row, "fallback", row.local_error);
			result.SetValue(row.row,
			                AiOptimizedClassifierResultValue(result_type, "", true, 0, false, ex.what(), metadata));
		}
	}

	RunProviderJobs(fallback_jobs, [&](ProviderStringJob &job) {
		AppendSystemPrompt(job.options, BuildTaskSystemPrompt(AiTaskKind::CLASSIFY, job.parameter));
		job.output =
		    duckdb_ai::Complete(BuildTaskUserPrompt(AiTaskKind::CLASSIFY, job.input, job.parameter), job.options).text;
	});
	for (auto &job : fallback_jobs) {
		auto &row = rows[fallback_row_indexes[job.row]];
		auto config = duckdb_ai::ResolveProvider(job.options);
		auto metadata = OptimizedClassifierMetadata(row, "fallback", row.local_error, config.provider, config.model);
		if (job.exception) {
			result.SetValue(job.row, AiOptimizedClassifierResultValue(result_type, "", true, 0, false,
			                                                          job.error_message, metadata));
			continue;
		}
		auto label = ClassifierLabelIndex(row.artifact.labels, job.output);
		if (label == DConstants::INVALID_INDEX) {
			result.SetValue(job.row, AiOptimizedClassifierResultValue(
			                             result_type, "", true, 0, false,
			                             "fallback classifier returned a label outside the artifact", metadata));
			continue;
		}
		result.SetValue(job.row, AiOptimizedClassifierResultValue(result_type, row.artifact.labels[label], true, 0,
		                                                          false, "", metadata));
	}
}

uint64_t StableTextHash(const std::string &input) {
	uint64_t hash = 1469598103934665603ULL;
	for (auto c : input) {
		hash ^= static_cast<unsigned char>(c);
		hash *= 1099511628211ULL;
	}
	return hash;
}

vector<idx_t> Utf8ByteOffsets(const std::string &input) {
	vector<idx_t> offsets;
	for (idx_t offset = 0; offset < input.size();) {
		offsets.push_back(offset);
		auto lead = static_cast<unsigned char>(input[offset]);
		idx_t width = 1;
		if ((lead & 0xe0) == 0xc0) {
			width = 2;
		} else if ((lead & 0xf0) == 0xe0) {
			width = 3;
		} else if ((lead & 0xf8) == 0xf0) {
			width = 4;
		}
		offset = MinValue<idx_t>(input.size(), offset + width);
	}
	offsets.push_back(input.size());
	return offsets;
}

bool IsChunkWhitespace(char value) {
	return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

idx_t FindRecursiveChunkEnd(const std::string &input, const vector<idx_t> &offsets, idx_t start, idx_t target) {
	auto minimum = start + MaxValue<idx_t>(1, (target - start) / 2);
	auto find_boundary = [&](uint8_t kind) {
		auto lower_bound = kind == 0 ? start : minimum;
		for (idx_t candidate = target; candidate > lower_bound; candidate--) {
			auto byte = offsets[candidate];
			if (kind == 0 && byte > 0 && input[byte - 1] == '\f') {
				return candidate;
			}
			if (kind == 0 && byte >= 2 && input[byte - 1] == '\n' && input[byte - 2] == '\n') {
				return candidate;
			}
			if (kind == 1 && byte > 0 && (input[byte - 1] == '.' || input[byte - 1] == '!' || input[byte - 1] == '?') &&
			    (byte == input.size() || IsChunkWhitespace(input[byte]))) {
				return candidate;
			}
			if (kind == 2 && byte > 0 && input[byte - 1] == '\n') {
				return candidate;
			}
			if (kind == 3 && byte > 0 && IsChunkWhitespace(input[byte - 1])) {
				return candidate;
			}
		}
		return idx_t(0);
	};
	for (uint8_t kind = 0; kind < 4; kind++) {
		auto boundary = find_boundary(kind);
		if (boundary > start) {
			return boundary;
		}
	}
	return target;
}

std::string MarkdownHeadingAt(const std::string &input, idx_t byte_offset) {
	std::string heading;
	for (idx_t line_start = 0; line_start <= byte_offset && line_start < input.size();) {
		auto newline = input.find('\n', line_start);
		auto page_break = input.find('\f', line_start);
		auto line_end = MinValue<idx_t>(newline == std::string::npos ? input.size() : newline,
		                                page_break == std::string::npos ? input.size() : page_break);
		if (line_end == std::string::npos) {
			line_end = input.size();
		}
		auto marker_end = line_start;
		while (marker_end < line_end && input[marker_end] == '#' && marker_end - line_start < 6) {
			marker_end++;
		}
		if (marker_end > line_start && marker_end < line_end && input[marker_end] == ' ') {
			heading = TrimAscii(input.substr(marker_end + 1, line_end - marker_end - 1));
		}
		if (line_end >= byte_offset || line_end == input.size()) {
			break;
		}
		line_start = line_end + 1;
	}
	return heading;
}

vector<AiTextChunk> BuildTextChunks(const std::string &input, std::string source_id, idx_t chunk_size,
                                    double overlap_percent, const std::string &strategy) {
	vector<AiTextChunk> chunks;
	if (input.empty()) {
		return chunks;
	}
	if (source_id.empty()) {
		source_id = StringUtil::Format("source_%016llx", static_cast<unsigned long long>(StableTextHash(input)));
	}
	auto offsets = Utf8ByteOffsets(input);
	auto codepoints = offsets.size() - 1;
	auto overlap = static_cast<idx_t>(std::floor(static_cast<double>(chunk_size) * overlap_percent / 100.0));
	auto settings_hash = StableTextHash(source_id + "|" + input + "|" + strategy + "|" + std::to_string(chunk_size) +
	                                    "|" + std::to_string(overlap_percent));
	for (idx_t start = 0, chunk_index = 0; start < codepoints; chunk_index++) {
		auto end = MinValue<idx_t>(codepoints, start + chunk_size);
		if (strategy == "recursive" && end < codepoints) {
			end = FindRecursiveChunkEnd(input, offsets, start, end);
		}
		if (end <= start) {
			end = MinValue<idx_t>(codepoints, start + chunk_size);
		}
		auto byte_start = offsets[start];
		auto byte_end = offsets[end];
		AiTextChunk chunk;
		chunk.source_id = source_id;
		chunk.chunk_id =
		    StringUtil::Format("%s:%016llx:%llu", source_id, static_cast<unsigned long long>(settings_hash),
		                       static_cast<unsigned long long>(chunk_index));
		chunk.chunk_index = chunk_index;
		chunk.start_offset = start;
		chunk.end_offset = end;
		chunk.text = input.substr(byte_start, byte_end - byte_start);
		chunk.heading = MarkdownHeadingAt(input, byte_start);
		chunk.page = 1 + NumericCast<idx_t>(std::count(input.begin(), input.begin() + byte_start, '\f'));
		chunks.push_back(std::move(chunk));
		if (end == codepoints) {
			break;
		}
		auto next_start = end > overlap ? end - overlap : end;
		start = next_start > start ? next_start : end;
	}
	return chunks;
}

std::string OptionalNamedString(const named_parameter_map_t &parameters, const std::string &name) {
	auto entry = parameters.find(name);
	if (entry == parameters.end() || entry->second.IsNull()) {
		return "";
	}
	auto value = entry->second;
	return StringValue::Get(value.DefaultCastAs(LogicalType::VARCHAR));
}

unique_ptr<FunctionData> AiChunkBindInternal(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names,
                                             bool prep_search) {
	if (input.inputs.size() != 1) {
		throw BinderException("%s requires one VARCHAR input", prep_search ? "ai_prep_search" : "ai_generate_chunks");
	}
	auto bind_data = make_uniq<AiChunkBindData>();
	bind_data->prep_search = prep_search;
	auto input_value = input.inputs[0];
	if (!input_value.IsNull()) {
		bind_data->input = StringValue::Get(input_value.DefaultCastAs(LogicalType::VARCHAR));
	}
	if (prep_search) {
		ApplySettings(context, bind_data->options, AiModelSettingKind::TASK);
		StampProviderFunction(bind_data->options, "ai_prep_search");
		duckdb_ai::AttachProviderRuntimeState(bind_data->options, context);
	}
	for (auto &parameter : input.named_parameters) {
		auto name = LowerAscii(parameter.first);
		auto value = parameter.second;
		if (name == "chunk_size") {
			if (value.IsNull()) {
				throw BinderException("chunk_size must not be NULL");
			}
			value = value.DefaultCastAs(LogicalType::BIGINT);
			auto chunk_size = BigIntValue::Get(value);
			if (chunk_size <= 0 || chunk_size > 10000000) {
				throw BinderException("chunk_size must be between 1 and 10000000");
			}
			bind_data->chunk_size = NumericCast<idx_t>(chunk_size);
		} else if (name == "overlap_percent") {
			value = value.DefaultCastAs(LogicalType::DOUBLE);
			bind_data->overlap_percent = DoubleValue::Get(value);
			if (!std::isfinite(bind_data->overlap_percent) || bind_data->overlap_percent < 0 ||
			    bind_data->overlap_percent > 50) {
				throw BinderException("overlap_percent must be between 0 and 50");
			}
		} else if (name == "strategy") {
			bind_data->strategy = LowerAscii(OptionalNamedString(input.named_parameters, parameter.first));
			if (bind_data->strategy != "fixed" && bind_data->strategy != "recursive") {
				throw BinderException("strategy must be fixed or recursive");
			}
		} else if (name == "source_id") {
			bind_data->source_id = OptionalNamedString(input.named_parameters, parameter.first);
		} else if (prep_search && name == "title") {
			bind_data->title = OptionalNamedString(input.named_parameters, parameter.first);
		} else if (prep_search && name == "metadata") {
			bind_data->metadata = OptionalNamedString(input.named_parameters, parameter.first);
			std::string error;
			if (!bind_data->metadata.empty() && !duckdb_ai::ValidateJsonDocument(bind_data->metadata, error)) {
				throw BinderException("ai_prep_search metadata must be valid JSON: %s", error);
			}
		} else if (prep_search && name == "enrich") {
			value = value.DefaultCastAs(LogicalType::BOOLEAN);
			bind_data->enrich = BooleanValue::Get(value);
		} else if (!prep_search ||
		           !ApplyCompletionValueOption(bind_data->options, "ai_prep_search", name, value, true, false)) {
			throw BinderException("Unsupported %s option \"%s\"", prep_search ? "ai_prep_search" : "ai_generate_chunks",
			                      name);
		}
	}
	if (bind_data->enrich) {
		ApplyAiProviderSecret(context, bind_data->options);
	}
	bind_data->chunks = BuildTextChunks(bind_data->input, bind_data->source_id, bind_data->chunk_size,
	                                    bind_data->overlap_percent, bind_data->strategy);
	if (!bind_data->chunks.empty()) {
		bind_data->source_id = bind_data->chunks[0].source_id;
	}
	if (prep_search) {
		AddTableColumns(return_types, names,
		                {{"source_id", LogicalType::VARCHAR},
		                 {"chunk_id", LogicalType::VARCHAR},
		                 {"chunk_index", LogicalType::UBIGINT},
		                 {"chunk_to_retrieve", LogicalType::VARCHAR},
		                 {"chunk_to_embed", LogicalType::VARCHAR},
		                 {"heading", LogicalType::VARCHAR},
		                 {"page", LogicalType::UBIGINT},
		                 {"start_offset", LogicalType::UBIGINT},
		                 {"end_offset", LogicalType::UBIGINT},
		                 {"metadata", LogicalType::JSON()},
		                 {"error", LogicalType::VARCHAR}});
	} else {
		AddTableColumns(return_types, names,
		                {{"source_id", LogicalType::VARCHAR},
		                 {"chunk_id", LogicalType::VARCHAR},
		                 {"chunk_index", LogicalType::UBIGINT},
		                 {"start_offset", LogicalType::UBIGINT},
		                 {"end_offset", LogicalType::UBIGINT},
		                 {"chunk_length", LogicalType::UBIGINT},
		                 {"estimated_tokens", LogicalType::BIGINT},
		                 {"chunk", LogicalType::VARCHAR}});
	}
	return std::move(bind_data);
}

unique_ptr<FunctionData> AiGenerateChunksBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	return AiChunkBindInternal(context, input, return_types, names, false);
}

unique_ptr<FunctionData> AiPrepSearchBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	return AiChunkBindInternal(context, input, return_types, names, true);
}

unique_ptr<GlobalTableFunctionState> AiChunkInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<AiChunkScanData>();
}

void PrepareSearchContext(ClientContext &context, const AiChunkBindData &bind_data, AiChunkScanData &state) {
	if (state.prepared) {
		return;
	}
	state.prepared = true;
	if (!bind_data.enrich) {
		duckdb_ai::RecordLocalUsageEvent(&context, "ai_prep_search", NumericCast<int64_t>(bind_data.input.size()),
		                                 NumericCast<int64_t>(bind_data.chunks.size()));
		return;
	}
	try {
		auto options = bind_data.options;
		AppendSystemPrompt(options,
		                   "Create a concise document-level retrieval context. Preserve the subject, entities, dates, "
		                   "and terminology needed to disambiguate individual chunks. Return only the context.");
		state.document_context = duckdb_ai::Complete(bind_data.input, options).text;
	} catch (std::exception &ex) {
		if (bind_data.options.fail_on_error) {
			throw;
		}
		state.enrichment_error = ex.what();
	}
}

std::string ChunkEmbeddingText(const AiChunkBindData &bind_data, const AiChunkScanData &state,
                               const AiTextChunk &chunk) {
	std::string output;
	if (!bind_data.title.empty()) {
		output += "Document: " + bind_data.title + "\n";
	}
	if (!state.document_context.empty()) {
		output += "Document context: " + state.document_context + "\n";
	}
	if (!chunk.heading.empty()) {
		output += "Section: " + chunk.heading + "\n";
	}
	if (!output.empty()) {
		output += "Content:\n";
	}
	output += chunk.text;
	return output;
}

void AiChunkFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<AiChunkBindData>();
	auto &data = data_p.global_state->Cast<AiChunkScanData>();
	if (bind_data.prep_search) {
		PrepareSearchContext(context, bind_data, data);
	} else if (!data.prepared) {
		duckdb_ai::RecordLocalUsageEvent(&context, "ai_generate_chunks", NumericCast<int64_t>(bind_data.input.size()),
		                                 NumericCast<int64_t>(bind_data.chunks.size()));
		data.prepared = true;
	}
	idx_t count = 0;
	while (data.offset < bind_data.chunks.size() && count < STANDARD_VECTOR_SIZE) {
		auto &chunk = bind_data.chunks[data.offset++];
		idx_t col = 0;
		output.SetValue(col++, count, Value(chunk.source_id));
		output.SetValue(col++, count, Value(chunk.chunk_id));
		output.SetValue(col++, count, Value::UBIGINT(chunk.chunk_index));
		if (bind_data.prep_search) {
			output.SetValue(col++, count, Value(chunk.text));
			output.SetValue(col++, count, Value(ChunkEmbeddingText(bind_data, data, chunk)));
			output.SetValue(col++, count, chunk.heading.empty() ? Value() : Value(chunk.heading));
			output.SetValue(col++, count, Value::UBIGINT(chunk.page));
			output.SetValue(col++, count, Value::UBIGINT(chunk.start_offset));
			output.SetValue(col++, count, Value::UBIGINT(chunk.end_offset));
			output.SetValue(col++, count, bind_data.metadata.empty() ? Value() : Value(bind_data.metadata));
			output.SetValue(col++, count, data.enrichment_error.empty() ? Value() : Value(data.enrichment_error));
		} else {
			output.SetValue(col++, count, Value::UBIGINT(chunk.start_offset));
			output.SetValue(col++, count, Value::UBIGINT(chunk.end_offset));
			output.SetValue(col++, count, Value::UBIGINT(chunk.end_offset - chunk.start_offset));
			output.SetValue(col++, count, Value::BIGINT(duckdb_ai::EstimateTokenCount(chunk.text)));
			output.SetValue(col++, count, Value(chunk.text));
		}
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> AiUsageBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                     vector<string> &names) {
	AddTableColumns(return_types, names,
	                {{"event_id", LogicalType::UBIGINT},
	                 {"created_at", LogicalType::VARCHAR},
	                 {"event", LogicalType::VARCHAR},
	                 {"function_name", LogicalType::VARCHAR},
	                 {"query_id", LogicalType::VARCHAR},
	                 {"operation_id", LogicalType::VARCHAR},
	                 {"parent_operation_id", LogicalType::VARCHAR},
	                 {"provider", LogicalType::VARCHAR},
	                 {"protocol", LogicalType::VARCHAR},
	                 {"model", LogicalType::VARCHAR},
	                 {"prompt_chars", LogicalType::BIGINT},
	                 {"response_chars", LogicalType::BIGINT},
	                 {"input_chars", LogicalType::BIGINT},
	                 {"dimensions", LogicalType::BIGINT},
	                 {"prompt_tokens", LogicalType::BIGINT},
	                 {"completion_tokens", LogicalType::BIGINT},
	                 {"total_tokens", LogicalType::BIGINT},
	                 {"cached_prompt_tokens", LogicalType::BIGINT},
	                 {"cache_creation_prompt_tokens", LogicalType::BIGINT},
	                 {"elapsed_ms", LogicalType::BIGINT},
	                 {"retries", LogicalType::BIGINT},
	                 {"http_status", LogicalType::BIGINT},
	                 {"cache_hit", LogicalType::BOOLEAN},
	                 {"status", LogicalType::VARCHAR},
	                 {"error", LogicalType::VARCHAR},
	                 {"estimated_cost_usd", LogicalType::DOUBLE}});
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> AiUsageInit(ClientContext &context, TableFunctionInitInput &) {
	return make_uniq<AiUsageScanData>(duckdb_ai::UsageEvents(context));
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
		output.SetValue(col++, count, event.function_name.empty() ? Value() : Value(event.function_name));
		output.SetValue(col++, count, event.query_id.empty() ? Value() : Value(event.query_id));
		output.SetValue(col++, count, event.operation_id.empty() ? Value() : Value(event.operation_id));
		output.SetValue(col++, count, event.parent_operation_id.empty() ? Value() : Value(event.parent_operation_id));
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
		output.SetValue(col++, count, Value::BIGINT(event.cached_prompt_tokens));
		output.SetValue(col++, count, Value::BIGINT(event.cache_creation_prompt_tokens));
		output.SetValue(col++, count, Value::BIGINT(event.elapsed_ms));
		output.SetValue(col++, count, Value::BIGINT(event.retries));
		output.SetValue(col++, count, Value::BIGINT(event.http_status));
		output.SetValue(col++, count, Value::BOOLEAN(event.cache_hit));
		output.SetValue(col++, count, Value(event.status));
		if (event.error.empty()) {
			output.SetValue(col++, count, Value());
		} else {
			output.SetValue(col++, count, Value(event.error));
		}
		if (event.estimated_cost_usd >= 0 && std::isfinite(event.estimated_cost_usd)) {
			output.SetValue(col++, count, Value::DOUBLE(event.estimated_cost_usd));
		} else {
			output.SetValue(col++, count, Value());
		}
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> AiUsageSummaryBind(ClientContext &, TableFunctionBindInput &,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	AddTableColumns(return_types, names,
	                {{"query_id", LogicalType::VARCHAR},
	                 {"provider", LogicalType::VARCHAR},
	                 {"model", LogicalType::VARCHAR},
	                 {"calls", LogicalType::UBIGINT},
	                 {"batch_count", LogicalType::UBIGINT},
	                 {"retries", LogicalType::UBIGINT},
	                 {"failures", LogicalType::UBIGINT},
	                 {"cache_hits", LogicalType::UBIGINT},
	                 {"total_tokens", LogicalType::BIGINT},
	                 {"elapsed_ms", LogicalType::BIGINT},
	                 {"estimated_cost_usd", LogicalType::DOUBLE},
	                 {"retained_events", LogicalType::UBIGINT},
	                 {"dropped_events", LogicalType::UBIGINT},
	                 {"queued_log_events", LogicalType::UBIGINT},
	                 {"dropped_log_events", LogicalType::UBIGINT}});
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> AiUsageSummaryInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<AiUsageSummaryScanData>();
	state->stats = duckdb_ai::UsageStats(context);
	auto events = duckdb_ai::UsageEvents(context);
	std::unordered_map<std::string, idx_t> indexes;
	for (auto &event : events) {
		auto query_id = event.query_id.empty() ? std::string("local") : event.query_id;
		auto entry = indexes.find(query_id);
		if (entry == indexes.end()) {
			AiUsageSummaryRow row;
			row.query_id = query_id;
			row.provider = event.provider;
			row.model = event.model;
			indexes[query_id] = state->rows.size();
			state->rows.push_back(std::move(row));
			entry = indexes.find(query_id);
		}
		auto &row = state->rows[entry->second];
		row.calls++;
		row.retries += NumericCast<uint64_t>(MaxValue<int64_t>(0, event.retries));
		if (!event.operation_id.empty()) {
			row.operation_ids.insert(event.operation_id);
		}
		row.failures += event.status == "error" ? 1 : 0;
		row.cache_hits += event.cache_hit ? 1 : 0;
		row.total_tokens += MaxValue<int64_t>(0, event.total_tokens);
		row.elapsed_ms += MaxValue<int64_t>(0, event.elapsed_ms);
		row.estimated_cost_usd += MaxValue<double>(0, event.estimated_cost_usd);
		if (row.provider != event.provider) {
			row.provider = "mixed";
		}
		if (row.model != event.model) {
			row.model = "mixed";
		}
	}
	if (state->rows.empty()) {
		AiUsageSummaryRow row;
		row.query_id = "local";
		row.provider = "local";
		state->rows.push_back(std::move(row));
	}
	return std::move(state);
}

void AiUsageSummaryFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<AiUsageSummaryScanData>();
	idx_t count = 0;
	while (data.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = data.rows[data.offset++];
		idx_t col = 0;
		output.SetValue(col++, count, Value(row.query_id));
		output.SetValue(col++, count, Value(row.provider));
		output.SetValue(col++, count, row.model.empty() ? Value() : Value(row.model));
		output.SetValue(col++, count, Value::UBIGINT(row.calls));
		output.SetValue(col++, count, Value::UBIGINT(row.operation_ids.size()));
		output.SetValue(col++, count, Value::UBIGINT(row.retries));
		output.SetValue(col++, count, Value::UBIGINT(row.failures));
		output.SetValue(col++, count, Value::UBIGINT(row.cache_hits));
		output.SetValue(col++, count, Value::BIGINT(row.total_tokens));
		output.SetValue(col++, count, Value::BIGINT(row.elapsed_ms));
		output.SetValue(col++, count, Value::DOUBLE(row.estimated_cost_usd));
		output.SetValue(col++, count, Value::UBIGINT(data.stats.retained_events));
		output.SetValue(col++, count, Value::UBIGINT(data.stats.dropped_events));
		output.SetValue(col++, count, Value::UBIGINT(data.stats.queued_log_events));
		output.SetValue(col++, count, Value::UBIGINT(data.stats.dropped_log_events));
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> AiClearUsageBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                          vector<string> &names) {
	AddTableColumns(return_types, names, {{"cleared", LogicalType::BOOLEAN}});
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> AiClearUsageInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<AiClearUsageScanData>();
}

void AiClearUsageFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<AiClearUsageScanData>();
	if (data.emitted) {
		return;
	}
	duckdb_ai::ClearUsageEvents(context);
	output.SetValue(0, 0, Value::BOOLEAN(true));
	output.SetCardinality(1);
	data.emitted = true;
}

void AiClearCacheFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<AiClearUsageScanData>();
	if (data.emitted) {
		return;
	}
	duckdb_ai::ClearResponseCache(context);
	ClearPromptQueryCache(context);
	ClearSimilarityCache(context);
	output.SetValue(0, 0, Value::BOOLEAN(true));
	output.SetCardinality(1);
	data.emitted = true;
}

unique_ptr<FunctionData> AiModelPricesBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           vector<string> &names) {
	AddTableColumns(return_types, names,
	                {{"provider", LogicalType::VARCHAR},
	                 {"model", LogicalType::VARCHAR},
	                 {"operation", LogicalType::VARCHAR},
	                 {"input_token_price_per_million", LogicalType::DOUBLE},
	                 {"output_token_price_per_million", LogicalType::DOUBLE},
	                 {"source_url", LogicalType::VARCHAR},
	                 {"source_note", LogicalType::VARCHAR},
	                 {"last_reviewed", LogicalType::VARCHAR}});
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
	AddSetting(config, prefix + "_allowed_hosts",
	           "Comma-separated AI provider host allowlist for " + label + "; empty allows all hosts",
	           LogicalType::VARCHAR, Value(""));
	AddSetting(config, prefix + "_cache", "Cache successful AI provider responses in the current DuckDB instance",
	           LogicalType::BOOLEAN, Value(LogicalType::BOOLEAN));
	AddSetting(config, prefix + "_cache_ttl_seconds",
	           "Maximum response-cache entry age in seconds between 0 and 31536000; 0 disables age expiry, -1 uses "
	           "default",
	           LogicalType::BIGINT, Value::BIGINT(-1));
	AddSetting(config, prefix + "_cache_max_entries",
	           "Maximum response-cache entries between 0 and 1000000; 0 disables response-cache storage, -1 uses "
	           "default",
	           LogicalType::BIGINT, Value::BIGINT(-1));
	AddSetting(config, prefix + "_prompt_cache", "Enable provider-side prompt caching hints when supported",
	           LogicalType::BOOLEAN, Value(LogicalType::BOOLEAN));
	AddSetting(config, prefix + "_response_format", "Default AI response format: text, json_object, or json_schema",
	           LogicalType::VARCHAR, Value(""));
	AddSetting(config, prefix + "_response_schema", "Default AI JSON schema object for structured responses",
	           LogicalType::VARCHAR, Value(""));
	AddSetting(config, prefix + "_timeout_seconds", "AI provider HTTP timeout in seconds; 0 uses the extension default",
	           LogicalType::BIGINT, Value::BIGINT(0));
	AddSetting(config, prefix + "_connect_timeout_seconds",
	           "AI provider connection timeout in seconds between 1 and 31536000; -1 uses default", LogicalType::BIGINT,
	           Value::BIGINT(-1));
	AddSetting(config, prefix + "_retry_count", "AI provider retry count between 0 and 10; -1 uses default",
	           LogicalType::BIGINT, Value::BIGINT(-1));
	AddSetting(config, prefix + "_retry_backoff_ms",
	           "AI provider retry backoff in milliseconds between 0 and 60000; -1 uses default", LogicalType::BIGINT,
	           Value::BIGINT(-1));
	AddSetting(config, prefix + "_max_concurrent_requests",
	           "Maximum concurrent AI provider requests between 0 and 64; 0 disables the limit, -1 uses default",
	           LogicalType::BIGINT, Value::BIGINT(-1));
	AddSetting(config, prefix + "_min_request_interval_ms",
	           "Minimum milliseconds between AI provider request starts between 0 and 60000; -1 uses default",
	           LogicalType::BIGINT, Value::BIGINT(-1));
	AddSetting(config, prefix + "_token_limit_per_minute",
	           "Maximum estimated AI provider tokens per rolling minute between 0 and 10000000000; 0 disables the "
	           "limit, -1 uses default",
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
	AddSetting(config, prefix + "_on_error", "AI error handling: fail, null, or capture", LogicalType::VARCHAR,
	           Value(""));
	AddSetting(config, prefix + "_log_include_text", "Include prompt and response text in AI usage logs",
	           LogicalType::BOOLEAN, Value(LogicalType::BOOLEAN));
	AddSetting(config, prefix + "_log_strict", "Fail SQL queries when AI usage log delivery fails",
	           LogicalType::BOOLEAN, Value(LogicalType::BOOLEAN));
}

void RegisterSettings(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	RegisterSettingsForPrefix(config, "duckdb_ai", "duckdb_ai");
	AddSetting(config, "duckdb_ai_completion_model", "Default AI model for completion functions", LogicalType::VARCHAR,
	           Value(""));
	AddSetting(config, "duckdb_ai_task_model", "Default AI model for text task functions", LogicalType::VARCHAR,
	           Value(""));
	AddSetting(config, "duckdb_ai_aggregate_model", "Default AI model for aggregate functions", LogicalType::VARCHAR,
	           Value(""));
	AddSetting(config, "duckdb_ai_embedding_model", "Default AI model for embedding functions", LogicalType::VARCHAR,
	           Value(""));
	AddSetting(config, "duckdb_ai_sql_assistant_model", "Default AI model for SQL assistant functions",
	           LogicalType::VARCHAR, Value(""));
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

	SecretType model_type;
	model_type.name = "duckdb_ai_model";
	model_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	model_type.default_provider = "config";
	model_type.extension = "duckdb_ai";
	loader.RegisterSecretType(std::move(model_type));

	CreateSecretFunction model_function;
	model_function.secret_type = "duckdb_ai_model";
	model_function.provider = "config";
	model_function.function = CreateAiModelSecretFromConfig;
	for (auto name : {"ai_provider", "model", "location", "credential", "model_type", "capabilities", "options"}) {
		model_function.named_parameters[name] = LogicalType::VARCHAR;
	}
	loader.RegisterFunction(std::move(model_function));
}

struct AiFunctionDocumentation {
	const char *name;
	const char *description;
	const char *example;
	const char *second_example;
};

static const AiFunctionDocumentation AI_FUNCTION_DOCUMENTATION[] = {
    {"ai_complete", "Calls a completion model and returns the response text.", "SELECT ai_complete('Say hello');",
     "SELECT ai_complete('Say hello', provider := 'ollama', model := 'llama3.2');"},
    {"ai_try_complete",
     "Calls a completion model and returns STRUCT(response, error) so row-level failures can be captured.",
     "SELECT ai_try_complete('Say hello');"},
    {"ai_complete_json", "Calls a completion model and validates the response as a JSON object or array.",
     "SELECT ai_complete_json('Return a JSON object with one key named ok');"},
    {"ai_complete_record",
     "Calls a completion model and projects a JSON object response into typed columns from a JSON Schema.",
     "SELECT * FROM ai_complete_record('Describe a duck', "
     "'{\"type\": \"object\", \"properties\": {\"name\": {\"type\": \"string\"}}}');"},
    {"ai_extract_record", "Extracts one typed STRUCT per row from text using a JSON Schema.",
     "SELECT ai_extract_record('Anna is 31', "
     "'{\"type\": \"object\", \"properties\": {\"age\": {\"type\": \"integer\"}}}');"},
    {"ai_completion_request_json", "Returns the completion request JSON without making a network call.",
     "SELECT ai_completion_request_json('Say hello');"},
    {"ai_embed", "Calls an embedding model and returns the embedding as DOUBLE[].", "SELECT ai_embed('duck');"},
    {"ai_embedding_request_json", "Returns the embedding request JSON without making a network call.",
     "SELECT ai_embedding_request_json('duck');"},
    {"ai_similarity", "Embeds two strings and returns their cosine similarity.",
     "SELECT ai_similarity('duck', 'goose');"},
    {"ai_rerank", "Uses a completion model to score candidate relevance to a query from 0 to 1.",
     "SELECT ai_rerank('best analytics database', 'DuckDB is an in-process analytics database');"},
    {"ai_score", "Uses a completion model to score how well input satisfies criteria from 0 to 1.",
     "SELECT ai_score('DuckDB runs in process', 'describes an embedded database');"},
    {"ai_summarize", "Summarizes text with a completion model.", "SELECT ai_summarize(review) FROM reviews;"},
    {"ai_sentiment", "Classifies text sentiment as positive, neutral, or negative.",
     "SELECT ai_sentiment(review) FROM reviews;"},
    {"ai_fix_grammar", "Rewrites text with corrected grammar, spelling, and punctuation.",
     "SELECT ai_fix_grammar('thes is a tst');"},
    {"ai_redact", "Masks direct personal data, credentials, secrets, and payment identifiers in text.",
     "SELECT ai_redact('Contact anna@example.com');"},
    {"ai_translate", "Translates text to the target language.", "SELECT ai_translate('Hello', 'French');"},
    {"ai_classify", "Chooses one label from a comma-separated VARCHAR or VARCHAR[] label list.",
     "SELECT ai_classify(review, ['positive', 'negative']) FROM reviews;",
     "SELECT ai_classify(review, 'positive, negative, mixed') FROM reviews;"},
    {"ai_classify_labels", "Chooses zero or more labels from a comma-separated VARCHAR or VARCHAR[] label list.",
     "SELECT ai_classify_labels(review, ['shipping', 'pricing', 'quality']) FROM reviews;"},
    {"ai_classify_result", "Classifies into zero or more labels and returns STRUCT(value, error, metadata).",
     "SELECT ai_classify_result(review, ['shipping', 'pricing', 'quality']) FROM reviews;"},
    {"ai_classify_optimized",
     "Uses a persisted centroid classifier artifact and falls back to an LLM when local confidence is insufficient.",
     "SELECT ai_classify_optimized(review, classifier) FROM reviews;"},
    {"ai_extract", "Extracts requested information from text.",
     "SELECT ai_extract('Anna is 31', 'the age of the person');"},
    {"ai_filter", "Evaluates a natural-language predicate against text and returns BOOLEAN.",
     "SELECT * FROM reviews WHERE ai_filter(review, 'mentions shipping problems');"},
    {"ai_agg", "Runs one completion over grouped text values and an instruction.",
     "SELECT ai_agg(review, 'List the top complaints') FROM reviews;"},
    {"ai_summarize_agg", "Summarizes grouped text values with one completion call.",
     "SELECT ai_summarize_agg(review) FROM reviews;"},
    {"ai_build_classifier",
     "Builds an experimental persisted classifier artifact using sampled LLM labels and batched embeddings.",
     "SELECT ai_build_classifier(review, ['positive', 'negative'], optimization := 'minimize_cost') FROM reviews;"},
    {"ai_sql",
     "Generates one read-only DuckDB SELECT statement from a natural-language question.\n"
     "With fix_attempts := N it verifies the SQL binds against the catalog and self-corrects using the bind error.",
     "SELECT ai_sql('total sales by region');",
     "SELECT ai_sql('total sales by region', include_tables := ['main.sales'], fix_attempts := 2);"},
    {"ai_query_data",
     "Generates one read-only SELECT at bind time and executes it as a subquery.\n"
     "With fix_attempts := N it verifies the SQL binds against the catalog and self-corrects using the bind error.",
     "SELECT * FROM ai_query_data('total sales by region');",
     "SELECT * FROM ai_query_data('total sales by region', include_tables := ['main.sales']);"},
    {"ai_schema_prompt", "Returns deterministic local catalog context for prompting SQL models.",
     "SELECT * FROM ai_schema_prompt();"},
    {"ai_explain_sql", "Explains one read-only DuckDB SELECT statement.", "SELECT * FROM ai_explain_sql('SELECT 42');"},
    {"ai_fix_sql",
     "Rewrites a broken query as one corrected read-only DuckDB SELECT, or rewrites one line with mode := 'line'.\n"
     "Accepts error := to pass the failure message and fix_attempts := N for bind-verified self-correction.",
     "SELECT * FROM ai_fix_sql('SELEC 42');",
     "SELECT * FROM ai_fix_sql('SELECT amout FROM sales', error := 'column amout not found', fix_attempts := 2);"},
    {"ai_is_read_only_sql",
     "Returns whether SQL is one parser-valid read-only SELECT statement.\n"
     "Pass true as the second argument to also check that the SQL binds against the catalog.",
     "SELECT ai_is_read_only_sql('SELECT 42');"},
    {"ai_validate_read_only_sql",
     "Returns normalized SQL or raises an error if it is not one read-only SELECT statement.\n"
     "Pass true as the second argument to also check that the SQL binds against the catalog.",
     "SELECT ai_validate_read_only_sql('SELECT 42');"},
    {"ai_count_tokens", "Returns a local approximate token count for text.", "SELECT ai_count_tokens('hello world');"},
    {"ai_recommended_batch_size", "Returns a conservative row batch size for rate-limited AI jobs.",
     "SELECT ai_recommended_batch_size(200, 100, 100000);"},
    {"ai_generate_chunks", "Splits text into deterministic fixed or recursive Unicode-aware chunks.",
     "SELECT * FROM ai_generate_chunks('First paragraph. Second paragraph.');"},
    {"ai_prep_search", "Creates retrieval and context-enriched embedding chunks from text or Markdown.",
     "SELECT * FROM ai_prep_search('# Guide\nDuckDB runs in process.', title := 'Guide');"},
    {"ai_provider_base_url", "Returns the default base URL for a supported provider.",
     "SELECT ai_provider_base_url('openai');"},
    {"ai_provider_protocol", "Returns the internal protocol used for a supported provider.",
     "SELECT ai_provider_protocol('openai');"},
    {"ai_usage", "Returns recent per-database AI usage events.", "SELECT * FROM ai_usage();"},
    {"ai_usage_summary", "Returns query-level AI usage totals and bounded-buffer drop counters.",
     "SELECT * FROM ai_usage_summary();"},
    {"ai_clear_usage", "Clears the per-database usage event buffer.", "SELECT * FROM ai_clear_usage();"},
    {"ai_clear_cache", "Clears per-database in-memory response and generated-SQL caches.",
     "SELECT * FROM ai_clear_cache();"},
    {"ai_secrets", "Lists configured duckdb_ai secrets with credentials redacted.", "SELECT * FROM ai_secrets();"},
    {"ai_models", "Lists registered external model profiles without credential material.",
     "SELECT * FROM ai_models();"},
    {"ai_provision_endpoint", "Plans or explicitly submits guarded endpoint provisioning through the control plane.",
     "SELECT * FROM ai_provision_endpoint('support_model');"},
    {"ai_endpoint_status", "Returns the current state of an asynchronous endpoint operation.",
     "SELECT * FROM ai_endpoint_status('operation-id');"},
    {"ai_deprovision_endpoint", "Explicitly submits endpoint deprovisioning through the control plane.",
     "SELECT * FROM ai_deprovision_endpoint('support_model');"},
    {"ai_parse_document", "Parses a BLOB through a normalized remote document-parser profile.",
     "SELECT * FROM ai_parse_document(read_blob('document.pdf').content, 'application/pdf', 'documents');"},
    {"ai_model_prices", "Returns the built-in provider/model pricing catalog.", "SELECT * FROM ai_model_prices();"},
};

void AttachAiFunctionDocumentation(CreateFunctionInfo &info) {
	for (auto &doc : AI_FUNCTION_DOCUMENTATION) {
		if (info.name == doc.name) {
			FunctionDescription description;
			description.description = doc.description;
			description.examples.emplace_back(doc.example);
			if (doc.second_example) {
				description.examples.emplace_back(doc.second_example);
			}
			info.descriptions.push_back(std::move(description));
			return;
		}
	}
	throw InternalException("Missing documentation entry for AI function %s", info.name);
}

void RegisterDocumentedFunction(ExtensionLoader &loader, ScalarFunctionSet set) {
	CreateScalarFunctionInfo info(std::move(set));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	AttachAiFunctionDocumentation(info);
	loader.RegisterFunction(std::move(info));
}

void RegisterDocumentedFunction(ExtensionLoader &loader, ScalarFunction function) {
	ScalarFunctionSet set(function.name);
	set.AddFunction(std::move(function));
	RegisterDocumentedFunction(loader, std::move(set));
}

void RegisterDocumentedFunction(ExtensionLoader &loader, AggregateFunctionSet set) {
	CreateAggregateFunctionInfo info(std::move(set));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	AttachAiFunctionDocumentation(info);
	loader.RegisterFunction(std::move(info));
}

void RegisterDocumentedFunction(ExtensionLoader &loader, TableFunctionSet set) {
	CreateTableFunctionInfo info(std::move(set));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	AttachAiFunctionDocumentation(info);
	loader.RegisterFunction(std::move(info));
}

void RegisterDocumentedFunction(ExtensionLoader &loader, TableFunction function) {
	TableFunctionSet set(function.name);
	set.AddFunction(std::move(function));
	RegisterDocumentedFunction(loader, std::move(set));
}

void RegisterTaskFunction(ExtensionLoader &loader, const std::string &name, vector<LogicalType> arguments,
                          bind_scalar_function_t bind) {
	auto function = ScalarFunction(name, std::move(arguments), LogicalType::VARCHAR, AiTaskFunction, bind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(function));
}

void RegisterClassifyFunction(ExtensionLoader &loader) {
	auto function = ScalarFunction("ai_classify", {LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::VARCHAR,
	                               AiTaskFunction, AiClassifyBind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(function));
}

void RegisterClassifyLabelsFunction(ExtensionLoader &loader) {
	auto function =
	    ScalarFunction("ai_classify_labels", {LogicalType::VARCHAR, LogicalType::ANY},
	                   LogicalType::LIST(LogicalType::VARCHAR), AiClassifyLabelsFunction, AiClassifyLabelsBind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(function));
}

void RegisterCompletionFunction(ExtensionLoader &loader, const std::string &name, bind_scalar_function_t bind) {
	auto function = ScalarFunction(name, {LogicalType::VARCHAR}, LogicalType::VARCHAR, AiCompletionFunction, bind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(function));
}

void RegisterTryCompletionFunction(ExtensionLoader &loader) {
	auto function = ScalarFunction("ai_try_complete", {LogicalType::VARCHAR}, AiTryCompleteReturnType(),
	                               AiTryCompleteFunction, AiTryCompleteBind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(function));
}

void RegisterEmbeddingFunction(ExtensionLoader &loader, const std::string &name) {
	auto function = ScalarFunction(name, {LogicalType::VARCHAR}, LogicalType::LIST(LogicalType::DOUBLE),
	                               AiEmbedFunction, AiEmbedBind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(function));
}

void RegisterAiFilterFunction(ExtensionLoader &loader) {
	auto function = ScalarFunction("ai_filter", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                               AiFilterFunction, AiFilterBind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(function));
}

void RegisterPromptSqlFunction(ExtensionLoader &loader, const std::string &name) {
	auto function =
	    ScalarFunction(name, {LogicalType::VARCHAR}, LogicalType::VARCHAR, AiPromptSqlFunction, AiPromptSqlBind);
	function.varargs = LogicalType::ANY;
	function.SetFallible();
	function.SetVolatile();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(function));
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
	RegisterDocumentedFunction(loader, std::move(functions));
}

void RegisterClassifierBuildFunction(ExtensionLoader &loader) {
	AggregateFunctionSet functions("ai_build_classifier");
	for (idx_t argument_count = 2; argument_count <= 32; argument_count++) {
		vector<LogicalType> arguments;
		arguments.emplace_back(LogicalType::VARCHAR);
		for (idx_t i = 1; i < argument_count; i++) {
			arguments.emplace_back(LogicalType::ANY);
		}
		auto function = AggregateFunction(
		    "ai_build_classifier", std::move(arguments), LogicalType::VARCHAR,
		    AggregateFunction::StateSize<ClassifierBuildState>,
		    AggregateFunction::StateInitialize<ClassifierBuildState, ClassifierBuildOperation>,
		    AggregateFunction::UnaryScatterUpdate<ClassifierBuildState, string_t, ClassifierBuildOperation>,
		    AggregateFunction::StateCombine<ClassifierBuildState, ClassifierBuildOperation>,
		    AggregateFunction::StateFinalize<ClassifierBuildState, string_t, ClassifierBuildOperation>,
		    FunctionNullHandling::DEFAULT_NULL_HANDLING,
		    AggregateFunction::UnaryUpdate<ClassifierBuildState, string_t, ClassifierBuildOperation>,
		    ClassifierBuildBind);
		function.SetFallible();
		function.SetVolatile();
		functions.AddFunction(std::move(function));
	}
	RegisterDocumentedFunction(loader, std::move(functions));
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
	                                           "token_limit_per_minute",
	                                           "cache",
	                                           "cache_ttl_seconds",
	                                           "cache_max_entries",
	                                           "prompt_cache",
	                                           "allowed_hosts",
	                                           "connect_timeout_seconds",
	                                           "input_token_price_per_million",
	                                           "output_token_price_per_million",
	                                           "use_builtin_model_prices",
	                                           "log_format",
	                                           "log_tags",
	                                           "log_sample_rate",
	                                           "on_error",
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

void AddPromptQueryNamedParameters(TableFunction &function, bool include_fix_attempts = true) {
	function.named_parameters["schema_context"] = LogicalType::VARCHAR;
	function.named_parameters["schema"] = LogicalType::VARCHAR;
	function.named_parameters["include_tables"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["exclude_tables"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["sample_rows"] = LogicalType::BIGINT;
	if (include_fix_attempts) {
		function.named_parameters["fix_attempts"] = LogicalType::BIGINT;
	}
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
	AddPromptQueryNamedParameters(function, include_error);
	if (include_error) {
		function.named_parameters["error"] = LogicalType::VARCHAR;
		function.named_parameters["mode"] = LogicalType::VARCHAR;
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
	RegisterDocumentedFunction(loader, std::move(ai_schema_prompt));
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
	RegisterDocumentedFunction(loader, std::move(ai_complete_record));

	auto ai_extract_record = ScalarFunction("ai_extract_record", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                        LogicalType::ANY, AiExtractRecordFunction, AiExtractRecordBind);
	ai_extract_record.varargs = LogicalType::ANY;
	ai_extract_record.SetFallible();
	ai_extract_record.SetVolatile();
	ai_extract_record.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(ai_extract_record));
}

void RegisterPromptExplainFunction(ExtensionLoader &loader, const std::string &name) {
	TableFunctionSet ai_explain_sql(name);
	TableFunction function({LogicalType::VARCHAR}, PromptAssistantFunction, PromptExplainBind, PromptAssistantInit);
	AddPromptAssistantNamedParameters(function, false);
	ai_explain_sql.AddFunction(std::move(function));
	RegisterDocumentedFunction(loader, std::move(ai_explain_sql));
}

void RegisterPromptFixupFunction(ExtensionLoader &loader, const std::string &name) {
	TableFunctionSet ai_fix_sql(name);
	vector<vector<LogicalType>> overloads = {
	    {LogicalType::VARCHAR},
	    {LogicalType::VARCHAR, LogicalType::VARCHAR},
	};
	for (auto &arguments : overloads) {
		TableFunction function(std::move(arguments), PromptAssistantFunction, PromptFixupBind, PromptAssistantInit);
		AddPromptAssistantNamedParameters(function, true);
		ai_fix_sql.AddFunction(std::move(function));
	}
	RegisterDocumentedFunction(loader, std::move(ai_fix_sql));
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
	RegisterDocumentedFunction(loader, std::move(ai_query_data));
}

struct ExternalModelParseData : public ParserExtensionParseData {
	std::string name;
	case_insensitive_map_t<std::string> options;
	bool replace = false;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto result = make_uniq<ExternalModelParseData>();
		result->name = name;
		result->options = options;
		result->replace = replace;
		return std::move(result);
	}

	string ToString() const override {
		return "CREATE EXTERNAL MODEL " + name;
	}
};

class ExternalModelSqlScanner {
public:
	explicit ExternalModelSqlScanner(const std::string &query_p) : query(query_p) {
	}

	void SkipWhitespace() {
		while (position < query.size() && std::isspace(static_cast<unsigned char>(query[position]))) {
			position++;
		}
	}

	bool ConsumeKeyword(const std::string &keyword) {
		SkipWhitespace();
		if (position + keyword.size() > query.size() ||
		    !StringUtil::CIEquals(query.substr(position, keyword.size()), keyword)) {
			return false;
		}
		auto end = position + keyword.size();
		if (end < query.size() && (std::isalnum(static_cast<unsigned char>(query[end])) || query[end] == '_')) {
			return false;
		}
		position = end;
		return true;
	}

	bool Consume(char expected) {
		SkipWhitespace();
		if (position >= query.size() || query[position] != expected) {
			return false;
		}
		position++;
		return true;
	}

	std::string Identifier() {
		SkipWhitespace();
		if (position >= query.size()) {
			throw ParserException("Expected an external model identifier");
		}
		if (query[position] == '"') {
			position++;
			std::string value;
			while (position < query.size()) {
				if (query[position] == '"') {
					if (position + 1 < query.size() && query[position + 1] == '"') {
						value.push_back('"');
						position += 2;
						continue;
					}
					position++;
					return value;
				}
				value.push_back(query[position++]);
			}
			throw ParserException("Unterminated quoted external model identifier");
		}
		auto start = position;
		while (position < query.size() &&
		       (std::isalnum(static_cast<unsigned char>(query[position])) || query[position] == '_')) {
			position++;
		}
		if (start == position) {
			throw ParserException("Expected an external model identifier");
		}
		return query.substr(start, position - start);
	}

	std::string ValueText() {
		SkipWhitespace();
		if (position >= query.size()) {
			throw ParserException("Expected an external model option value");
		}
		if (query[position] == '\'') {
			position++;
			std::string value;
			while (position < query.size()) {
				if (query[position] == '\'') {
					if (position + 1 < query.size() && query[position + 1] == '\'') {
						value.push_back('\'');
						position += 2;
						continue;
					}
					position++;
					return value;
				}
				value.push_back(query[position++]);
			}
			throw ParserException("Unterminated external model option string");
		}
		auto start = position;
		while (position < query.size() && query[position] != ',' && query[position] != ')') {
			position++;
		}
		auto value = query.substr(start, position - start);
		StringUtil::Trim(value);
		if (value.empty()) {
			throw ParserException("Expected an external model option value");
		}
		return value;
	}

	bool Finished() {
		SkipWhitespace();
		if (position < query.size() && query[position] == ';') {
			position++;
			SkipWhitespace();
		}
		return position == query.size();
	}

private:
	const std::string &query;
	idx_t position = 0;
};

class ExternalModelParserExtension : public ParserExtension {
public:
	ExternalModelParserExtension() {
		parse_function = Parse;
		plan_function = Plan;
	}

	static ParserExtensionParseResult Parse(ParserExtensionInfo *, const string &query) {
		try {
			ExternalModelSqlScanner scanner(query);
			if (!scanner.ConsumeKeyword("create")) {
				return ParserExtensionParseResult();
			}
			auto data = make_uniq<ExternalModelParseData>();
			if (scanner.ConsumeKeyword("or")) {
				if (!scanner.ConsumeKeyword("replace")) {
					return ParserExtensionParseResult();
				}
				data->replace = true;
			}
			if (!scanner.ConsumeKeyword("external") || !scanner.ConsumeKeyword("model")) {
				return ParserExtensionParseResult();
			}
			data->name = scanner.Identifier();
			if (!scanner.ConsumeKeyword("with") || !scanner.Consume('(')) {
				return ParserExtensionParseResult("CREATE EXTERNAL MODEL requires WITH (...)");
			}
			while (true) {
				auto key = LowerAscii(scanner.Identifier());
				if (!scanner.Consume('=')) {
					return ParserExtensionParseResult("Expected '=' after external model option " + key);
				}
				if (key == "base_url" || key == "endpoint") {
					key = "location";
				}
				static const std::set<std::string> supported = {"provider",   "model",        "location", "credential",
				                                                "model_type", "capabilities", "options"};
				if (supported.find(key) == supported.end()) {
					return ParserExtensionParseResult("Unsupported external model option: " + key);
				}
				if (data->options.find(key) != data->options.end()) {
					return ParserExtensionParseResult("Duplicate external model option: " + key);
				}
				data->options[key] = scanner.ValueText();
				if (scanner.Consume(')')) {
					break;
				}
				if (!scanner.Consume(',')) {
					return ParserExtensionParseResult("Expected ',' or ')' in external model options");
				}
			}
			if (!scanner.Finished()) {
				return ParserExtensionParseResult("Unexpected text after CREATE EXTERNAL MODEL");
			}
			if (data->options.find("provider") == data->options.end() ||
			    data->options.find("model") == data->options.end()) {
				return ParserExtensionParseResult("CREATE EXTERNAL MODEL requires provider and model options");
			}
			return ParserExtensionParseResult(std::move(data));
		} catch (std::exception &ex) {
			return ParserExtensionParseResult(ex.what());
		}
	}

	static ParserExtensionPlanResult Plan(ParserExtensionInfo *, ClientContext &,
	                                      unique_ptr<ParserExtensionParseData> parse_data) {
		auto &data = static_cast<ExternalModelParseData &>(*parse_data);
		auto get = [&](const std::string &key) {
			auto entry = data.options.find(key);
			return entry == data.options.end() ? std::string() : entry->second;
		};
		ParserExtensionPlanResult result;
		result.function = ExternalModelTableFunction();
		result.parameters = {Value(data.name),           Value(get("provider")),   Value(get("model")),
		                     Value(get("location")),     Value(get("credential")), Value(get("model_type")),
		                     Value(get("capabilities")), Value(get("options")),    Value::BOOLEAN(data.replace)};
		result.return_type = StatementReturnType::QUERY_RESULT;
		return result;
	}
};

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	duckdb_ai::InitializeProviderRuntime();
	RegisterSettings(loader);
	RegisterAiProviderSecret(loader);
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	ParserExtension::Register(config, ExternalModelParserExtension());

	RegisterCompletionFunction(loader, "ai_complete", AiCompleteBind);
	RegisterTryCompletionFunction(loader);
	RegisterCompletionFunction(loader, "ai_completion_request_json", AiCompletionRequestJsonBind);

	auto ai_complete_json = ScalarFunction("ai_complete_json", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                       AiCompletionFunction, AiCompleteJsonBind);
	ai_complete_json.varargs = LogicalType::ANY;
	ai_complete_json.SetFallible();
	ai_complete_json.SetVolatile();
	ai_complete_json.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(ai_complete_json));
	RegisterAiRecordFunction(loader);

	RegisterEmbeddingFunction(loader, "ai_embed");

	auto ai_embedding_request_json =
	    ScalarFunction("ai_embedding_request_json", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                   AiEmbeddingRequestJsonFunction, AiEmbeddingRequestJsonBind);
	ai_embedding_request_json.varargs = LogicalType::ANY;
	ai_embedding_request_json.SetFallible();
	ai_embedding_request_json.SetVolatile();
	ai_embedding_request_json.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(ai_embedding_request_json));

	auto ai_similarity = ScalarFunction("ai_similarity", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                    LogicalType::DOUBLE, AiSimilarityFunction, AiSimilarityBind);
	ai_similarity.varargs = LogicalType::ANY;
	ai_similarity.SetFallible();
	ai_similarity.SetVolatile();
	ai_similarity.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(ai_similarity));

	auto ai_rerank = ScalarFunction("ai_rerank", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::DOUBLE,
	                                AiRerankFunction, AiRerankBind);
	ai_rerank.varargs = LogicalType::ANY;
	ai_rerank.SetFallible();
	ai_rerank.SetVolatile();
	ai_rerank.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(ai_rerank));

	auto ai_score = ScalarFunction("ai_score", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::DOUBLE,
	                               AiRerankFunction, AiRerankBind);
	ai_score.varargs = LogicalType::ANY;
	ai_score.SetFallible();
	ai_score.SetVolatile();
	ai_score.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(ai_score));

	RegisterTaskFunction(loader, "ai_summarize", {LogicalType::VARCHAR}, AiSummarizeBind);
	RegisterTaskFunction(loader, "ai_sentiment", {LogicalType::VARCHAR}, AiSentimentBind);
	RegisterTaskFunction(loader, "ai_fix_grammar", {LogicalType::VARCHAR}, AiFixGrammarBind);
	RegisterTaskFunction(loader, "ai_redact", {LogicalType::VARCHAR}, AiRedactBind);
	RegisterTaskFunction(loader, "ai_translate", {LogicalType::VARCHAR, LogicalType::VARCHAR}, AiTranslateBind);
	RegisterClassifyFunction(loader);
	RegisterClassifyLabelsFunction(loader);
	auto ai_classify_result =
	    ScalarFunction("ai_classify_result", {LogicalType::VARCHAR, LogicalType::ANY}, AiClassifyResultReturnType(),
	                   AiClassifyResultFunction, AiClassifyResultBind);
	ai_classify_result.varargs = LogicalType::ANY;
	ai_classify_result.SetFallible();
	ai_classify_result.SetVolatile();
	ai_classify_result.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(ai_classify_result));
	auto ai_classify_optimized =
	    ScalarFunction("ai_classify_optimized", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                   AiOptimizedClassifierReturnType(), AiOptimizedClassifierFunction, AiOptimizedClassifierBind);
	ai_classify_optimized.varargs = LogicalType::ANY;
	ai_classify_optimized.SetFallible();
	ai_classify_optimized.SetVolatile();
	ai_classify_optimized.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(ai_classify_optimized));
	RegisterTaskFunction(loader, "ai_extract", {LogicalType::VARCHAR, LogicalType::VARCHAR}, AiExtractBind);
	RegisterAiFilterFunction(loader);

	RegisterAiAggregateFunction(loader, "ai_agg", AiAggBind, 2, 10);
	RegisterAiAggregateFunction(loader, "ai_summarize_agg", AiSummarizeAggBind, 1, 9);
	RegisterClassifierBuildFunction(loader);

	auto ai_generate_chunks =
	    TableFunction("ai_generate_chunks", {LogicalType::VARCHAR}, AiChunkFunction, AiGenerateChunksBind, AiChunkInit);
	ai_generate_chunks.named_parameters["chunk_size"] = LogicalType::BIGINT;
	ai_generate_chunks.named_parameters["overlap_percent"] = LogicalType::DOUBLE;
	ai_generate_chunks.named_parameters["strategy"] = LogicalType::VARCHAR;
	ai_generate_chunks.named_parameters["source_id"] = LogicalType::VARCHAR;
	RegisterDocumentedFunction(loader, std::move(ai_generate_chunks));

	auto ai_prep_search =
	    TableFunction("ai_prep_search", {LogicalType::VARCHAR}, AiChunkFunction, AiPrepSearchBind, AiChunkInit);
	ai_prep_search.named_parameters["chunk_size"] = LogicalType::BIGINT;
	ai_prep_search.named_parameters["overlap_percent"] = LogicalType::DOUBLE;
	ai_prep_search.named_parameters["strategy"] = LogicalType::VARCHAR;
	ai_prep_search.named_parameters["source_id"] = LogicalType::VARCHAR;
	ai_prep_search.named_parameters["title"] = LogicalType::VARCHAR;
	ai_prep_search.named_parameters["metadata"] = LogicalType::VARCHAR;
	ai_prep_search.named_parameters["enrich"] = LogicalType::BOOLEAN;
	AddCompletionNamedParameters(ai_prep_search, false, false);
	RegisterDocumentedFunction(loader, std::move(ai_prep_search));

	auto ai_parse_document =
	    TableFunction("ai_parse_document", {LogicalType::BLOB, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                  DocumentParseFunction, DocumentParseBind, DocumentParseInit);
	ai_parse_document.named_parameters["pages"] = LogicalType::VARCHAR;
	AddCompletionNamedParameters(ai_parse_document, false, false);
	RegisterDocumentedFunction(loader, std::move(ai_parse_document));

	RegisterPromptSqlFunction(loader, "ai_sql");

	RegisterPromptSchemaFunction(loader, "ai_schema_prompt");
	RegisterPromptExplainFunction(loader, "ai_explain_sql");
	RegisterPromptFixupFunction(loader, "ai_fix_sql");
	RegisterPromptQueryFunction(loader, "ai_query_data");

	ScalarFunctionSet ai_is_read_only_sql("ai_is_read_only_sql");
	ai_is_read_only_sql.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR}, LogicalType::BOOLEAN, AiIsReadOnlySqlFunction));
	ai_is_read_only_sql.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::BOOLEAN}, LogicalType::BOOLEAN, AiIsReadOnlySqlFunction));
	RegisterDocumentedFunction(loader, std::move(ai_is_read_only_sql));
	ScalarFunctionSet ai_validate_read_only_sql("ai_validate_read_only_sql");
	ai_validate_read_only_sql.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, AiValidateReadOnlySqlFunction));
	ai_validate_read_only_sql.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::BOOLEAN},
	                                                     LogicalType::VARCHAR, AiValidateReadOnlySqlFunction));
	RegisterDocumentedFunction(loader, std::move(ai_validate_read_only_sql));
	RegisterDocumentedFunction(loader, ScalarFunction("ai_provider_base_url", {LogicalType::VARCHAR},
	                                                  LogicalType::VARCHAR, AiProviderBaseUrl));
	RegisterDocumentedFunction(loader, ScalarFunction("ai_provider_protocol", {LogicalType::VARCHAR},
	                                                  LogicalType::VARCHAR, AiProviderProtocol));

	auto ai_count_tokens = ScalarFunction("ai_count_tokens", {LogicalType::VARCHAR}, LogicalType::BIGINT,
	                                      AiCountTokensFunction, AiCountTokensBind);
	ai_count_tokens.varargs = LogicalType::ANY;
	ai_count_tokens.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	RegisterDocumentedFunction(loader, std::move(ai_count_tokens));

	ScalarFunctionSet ai_recommended_batch_size("ai_recommended_batch_size");
	for (idx_t argument_count = 3; argument_count <= 5; argument_count++) {
		vector<LogicalType> arguments;
		for (idx_t i = 0; i < argument_count; i++) {
			arguments.emplace_back(LogicalType::DOUBLE);
		}
		auto function = ScalarFunction(std::move(arguments), LogicalType::BIGINT, AiRecommendedBatchSizeFunction);
		function.SetFallible();
		function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		ai_recommended_batch_size.AddFunction(std::move(function));
	}
	RegisterDocumentedFunction(loader, std::move(ai_recommended_batch_size));

	RegisterDocumentedFunction(loader, TableFunction("ai_usage", {}, AiUsageFunction, AiUsageBind, AiUsageInit));
	RegisterDocumentedFunction(
	    loader, TableFunction("ai_usage_summary", {}, AiUsageSummaryFunction, AiUsageSummaryBind, AiUsageSummaryInit));
	RegisterDocumentedFunction(
	    loader, TableFunction("ai_clear_usage", {}, AiClearUsageFunction, AiClearUsageBind, AiClearUsageInit));
	RegisterDocumentedFunction(
	    loader, TableFunction("ai_clear_cache", {}, AiClearCacheFunction, AiClearUsageBind, AiClearUsageInit));
	RegisterDocumentedFunction(loader,
	                           TableFunction("ai_secrets", {}, AiSecretsFunction, AiSecretsBind, AiSecretsInit));
	RegisterDocumentedFunction(loader, TableFunction("ai_models", {}, AiModelsFunction, AiModelsBind, AiModelsInit));
	auto ai_provision_endpoint = TableFunction("ai_provision_endpoint", {LogicalType::VARCHAR}, ControlPlaneFunction,
	                                           ProvisionEndpointBind, AiClearUsageInit);
	ai_provision_endpoint.named_parameters["dry_run"] = LogicalType::BOOLEAN;
	ai_provision_endpoint.named_parameters["max_hourly_cost_usd"] = LogicalType::DOUBLE;
	RegisterDocumentedFunction(loader, std::move(ai_provision_endpoint));
	RegisterDocumentedFunction(loader, TableFunction("ai_endpoint_status", {LogicalType::VARCHAR}, ControlPlaneFunction,
	                                                 EndpointStatusBind, AiClearUsageInit));
	RegisterDocumentedFunction(loader, TableFunction("ai_deprovision_endpoint", {LogicalType::VARCHAR},
	                                                 ControlPlaneFunction, DeprovisionEndpointBind, AiClearUsageInit));
	RegisterDocumentedFunction(
	    loader, TableFunction("ai_model_prices", {}, AiModelPricesFunction, AiModelPricesBind, AiModelPricesInit));
}

void AiExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string AiExtension::Name() {
	return "ai";
}

std::string AiExtension::Version() const {
#ifdef EXT_VERSION_AI
	return EXT_VERSION_AI;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(ai, loader) {
	duckdb::LoadInternal(loader);
}
}
