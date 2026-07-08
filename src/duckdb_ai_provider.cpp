#include "duckdb_ai_provider.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <curl/curl.h>
#include <deque>
#include <list>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb {
namespace duckdb_ai {

namespace {

struct HttpResponse {
	HttpResponse() : status(0), elapsed_ms(0), retry_after_ms(-1), retries(0), cache_hit(false) {
	}

	HttpResponse(std::string body_p, long status_p, int64_t elapsed_ms_p, long retry_after_ms_p)
	    : body(std::move(body_p)), status(status_p), elapsed_ms(elapsed_ms_p), retry_after_ms(retry_after_ms_p),
	      retries(0), cache_hit(false) {
	}

	std::string body;
	long status;
	int64_t elapsed_ms;
	long retry_after_ms;
	int64_t retries;
	bool cache_hit;
};

struct CachedHttpResponse {
	HttpResponse response;
	std::chrono::steady_clock::time_point created_at;
	//! Position in ProviderRuntimeState::response_cache_order for O(1) LRU updates.
	std::list<std::string>::iterator order_entry;
};

struct PendingHttpResponse {
	std::condition_variable cv;
	bool done = false;
	HttpResponse response;
	std::string error;
};

struct UsageLogJob {
	std::string url;
	std::string payload;
	std::vector<std::string> headers;
	long timeout_seconds;
	long retry_count;
	long retry_backoff_ms;
};

struct ProviderRuntimeState;
void StopUsageLogWorker(ProviderRuntimeState &state);

const size_t MAX_USAGE_EVENTS = 1024;
const size_t MAX_USAGE_LOG_QUEUE = 4096;
//! Maximum inputs per batched embedding request. Keeps request payloads bounded and stays well
//! below provider per-request input limits (OpenAI allows up to 2048 inputs).
const size_t MAX_EMBED_BATCH_INPUTS = 512;
constexpr int64_t MAX_TOKEN_LIMIT_PER_MINUTE = 10000000000LL;
constexpr int64_t MAX_PROVIDER_CHUNK_WORKERS = 64;
constexpr int64_t DEFAULT_COMPLETION_OUTPUT_TOKEN_ESTIMATE = 512;
std::once_flag curl_global_init_once;
const size_t DEFAULT_MAX_RESPONSE_CACHE_ENTRIES = 1024;

struct ProviderRuntimeState : public ObjectCacheEntry {
	~ProviderRuntimeState();

	static std::string ObjectType() {
		return "duckdb_ai_runtime_state";
	}

	std::string GetObjectType() override {
		return ObjectType();
	}

	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}

	std::mutex usage_mutex;
	std::vector<UsageEvent> usage_events;
	uint64_t next_usage_event_id = 1;

	std::mutex provider_control_mutex;
	std::condition_variable provider_control_cv;
	int64_t active_provider_requests = 0;
	bool has_last_provider_request_start = false;
	std::chrono::steady_clock::time_point last_provider_request_start;
	std::deque<std::pair<std::chrono::steady_clock::time_point, int64_t>> provider_token_window;
	int64_t provider_token_window_tokens = 0;

	std::mutex response_cache_mutex;
	std::unordered_map<std::string, CachedHttpResponse> response_cache;
	//! Least-recently-used cache keys first; entries hold their own iterator for O(1) reordering.
	std::list<std::string> response_cache_order;
	std::unordered_map<std::string, std::shared_ptr<PendingHttpResponse>> response_cache_pending;

	std::mutex usage_log_mutex;
	std::condition_variable usage_log_cv;
	std::deque<UsageLogJob> usage_log_queue;
	std::thread usage_log_worker;
	bool usage_log_worker_started = false;
	bool usage_log_shutdown = false;
};

ProviderRuntimeState fallback_runtime_state;

ProviderRuntimeState &RuntimeState(ClientContext &context) {
	return *ObjectCache::GetObjectCache(context).GetOrCreate<ProviderRuntimeState>(ProviderRuntimeState::ObjectType());
}

ProviderRuntimeState &RuntimeState(const CompletionOptions &options) {
	if (options.runtime_state) {
		return *reinterpret_cast<ProviderRuntimeState *>(options.runtime_state);
	}
	return options.client_context ? RuntimeState(*options.client_context) : fallback_runtime_state;
}

std::string LowerAscii(std::string input) {
	std::transform(input.begin(), input.end(), input.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return input;
}

std::string NormalizeProviderNameInternal(const std::string &provider_input) {
	auto provider = LowerAscii(provider_input);
	if (provider == "claude") {
		return "anthropic";
	}
	if (provider == "gcp" || provider == "google" || provider == "google_gemini") {
		return "gemini";
	}
	if (provider == "zhipu") {
		return "zai";
	}
	if (provider == "azure_openai" || provider == "azure-openai") {
		return "azure";
	}
	if (provider == "mosaic" || provider == "mosaic_ai" || provider == "databricks_ai") {
		return "databricks";
	}
	if (provider == "privacy_filter" || provider == "openai_privacy_filter" || provider == "pii_filter" ||
	    provider == "opf") {
		return "openai_privacy_filter";
	}
	if (provider == "openai-compatible" || provider == "openai_compatible" || provider == "local" ||
	    provider == "local_openai" || provider == "local-models" || provider == "local_models") {
		return "openai_compatible";
	}
	if (provider == "llama.cpp" || provider == "llama-cpp" || provider == "llama_cpp" || provider == "llama-server" ||
	    provider == "llama_server") {
		return "llamacpp";
	}
	return provider;
}

std::string GetEnv(const std::string &name) {
	const char *value = std::getenv(name.c_str());
	return value ? std::string(value) : std::string();
}

bool EnvFlagEnabled(const std::string &name) {
	auto value = LowerAscii(GetEnv(name));
	return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool TryReadEnvFlag(const std::string &name, bool &target) {
	auto value = LowerAscii(GetEnv(name));
	if (value.empty()) {
		return false;
	}
	if (value == "1" || value == "true" || value == "yes" || value == "on") {
		target = true;
		return true;
	}
	if (value == "0" || value == "false" || value == "no" || value == "off") {
		target = false;
		return true;
	}
	throw InvalidInputException("%s must be a boolean", name);
}

bool TryReadEnvInt64(const std::string &name, int64_t &target, int64_t min_value, int64_t max_value) {
	auto configured = GetEnv(name);
	if (configured.empty()) {
		return false;
	}
	try {
		auto value = std::stoll(configured);
		if (value < min_value || value > max_value) {
			throw InvalidInputException("%s must be between %lld and %lld", name, static_cast<long long>(min_value),
			                            static_cast<long long>(max_value));
		}
		target = value;
		return true;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("%s must be an integer between %lld and %lld", name,
		                            static_cast<long long>(min_value), static_cast<long long>(max_value));
	}
}

bool TryReadEnvDouble(const std::string &name, double &target, double min_value, double max_value) {
	auto configured = GetEnv(name);
	if (configured.empty()) {
		return false;
	}
	try {
		auto value = std::stod(configured);
		if (!std::isfinite(value) || value < min_value || value > max_value) {
			throw InvalidInputException("%s must be between %.0f and %.0f", name, min_value, max_value);
		}
		target = value;
		return true;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("%s must be a number between %.0f and %.0f", name, min_value, max_value);
	}
}

std::string TrimTrailingSlash(std::string value) {
	while (value.size() > 1 && value.back() == '/') {
		value.pop_back();
	}
	return value;
}

std::string TrimAscii(const std::string &value) {
	size_t start = 0;
	while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
		start++;
	}
	size_t end = value.size();
	while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
		end--;
	}
	return value.substr(start, end - start);
}

std::string UrlHost(const std::string &url) {
	auto scheme_end = url.find("://");
	if (scheme_end == std::string::npos) {
		throw InvalidInputException("AI provider URL must include a scheme: %s", url);
	}
	auto host_start = scheme_end + 3;
	auto path_start = url.find('/', host_start);
	auto authority =
	    url.substr(host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
	auto at_pos = authority.rfind('@');
	if (at_pos != std::string::npos) {
		authority = authority.substr(at_pos + 1);
	}
	if (authority.empty()) {
		throw InvalidInputException("AI provider URL must include a host: %s", url);
	}
	if (authority.front() == '[') {
		auto bracket_end = authority.find(']');
		if (bracket_end == std::string::npos) {
			throw InvalidInputException("AI provider URL has an invalid IPv6 host: %s", url);
		}
		return LowerAscii(authority.substr(1, bracket_end - 1));
	}
	auto colon_pos = authority.find(':');
	auto host = colon_pos == std::string::npos ? authority : authority.substr(0, colon_pos);
	if (host.empty()) {
		throw InvalidInputException("AI provider URL must include a host: %s", url);
	}
	return LowerAscii(host);
}

bool HostMatchesAllowedPattern(const std::string &host, const std::string &pattern) {
	if (pattern.empty()) {
		return false;
	}
	if (pattern == "*") {
		return true;
	}
	if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
		auto suffix = pattern.substr(1);
		return host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
	}
	return host == pattern;
}

std::string NormalizeAllowedHostPattern(std::string token) {
	token = LowerAscii(std::move(token));
	if (token == "*" || (token.size() > 2 && token[0] == '*' && token[1] == '.')) {
		return token;
	}
	if (token.find("://") != std::string::npos) {
		return UrlHost(token);
	}
	auto slash = token.find('/');
	if (slash != std::string::npos) {
		token = token.substr(0, slash);
	}
	if (!token.empty() && token.front() == '[') {
		auto bracket_end = token.find(']');
		if (bracket_end != std::string::npos) {
			return token.substr(1, bracket_end - 1);
		}
	}
	auto colon = token.find(':');
	if (colon != std::string::npos && token.find(':', colon + 1) == std::string::npos) {
		token = token.substr(0, colon);
	}
	return token;
}

std::vector<std::string> SplitCommaList(const std::string &input) {
	std::vector<std::string> values;
	size_t pos = 0;
	while (pos <= input.size()) {
		auto comma = input.find(',', pos);
		auto token = TrimAscii(input.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
		if (!token.empty()) {
			values.push_back(NormalizeAllowedHostPattern(std::move(token)));
		}
		if (comma == std::string::npos) {
			break;
		}
		pos = comma + 1;
	}
	return values;
}

std::string AllowedHosts(const CompletionOptions &options) {
	if (!options.allowed_hosts.empty()) {
		return options.allowed_hosts;
	}
	return GetEnv("DUCKDB_AI_ALLOWED_HOSTS");
}

void EnforceAllowedHost(const std::string &url, const CompletionOptions &options) {
	auto allowed_hosts = SplitCommaList(AllowedHosts(options));
	if (allowed_hosts.empty()) {
		return;
	}
	auto host = UrlHost(url);
	for (auto &allowed_host : allowed_hosts) {
		if (HostMatchesAllowedPattern(host, allowed_host)) {
			return;
		}
	}
	throw InvalidInputException("AI provider host \"%s\" is not allowed by duckdb_ai_allowed_hosts", host);
}

void EnsureCurlGlobalInit() {
	std::call_once(curl_global_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct CurlShareLocks {
	// libcurl may lock different data classes independently (and could nest them), so use one
	// mutex per curl_lock_data value instead of a single non-recursive mutex for everything.
	std::mutex mutexes[CURL_LOCK_DATA_LAST];

	std::mutex &MutexFor(curl_lock_data data) {
		auto index = static_cast<size_t>(data);
		if (index >= CURL_LOCK_DATA_LAST) {
			index = 0;
		}
		return mutexes[index];
	}
};

void CurlShareLock(CURL *, curl_lock_data data, curl_lock_access, void *userptr) {
	reinterpret_cast<CurlShareLocks *>(userptr)->MutexFor(data).lock();
}

void CurlShareUnlock(CURL *, curl_lock_data data, void *userptr) {
	reinterpret_cast<CurlShareLocks *>(userptr)->MutexFor(data).unlock();
}

struct CurlShareHandle {
	CurlShareHandle() {
		EnsureCurlGlobalInit();
		handle = curl_share_init();
		if (!handle) {
			throw IOException("Could not initialize libcurl share handle for AI requests");
		}
		if (curl_share_setopt(handle, CURLSHOPT_LOCKFUNC, CurlShareLock) != CURLSHE_OK ||
		    curl_share_setopt(handle, CURLSHOPT_UNLOCKFUNC, CurlShareUnlock) != CURLSHE_OK ||
		    curl_share_setopt(handle, CURLSHOPT_USERDATA, &locks) != CURLSHE_OK ||
		    curl_share_setopt(handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT) != CURLSHE_OK ||
		    curl_share_setopt(handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS) != CURLSHE_OK ||
		    curl_share_setopt(handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION) != CURLSHE_OK) {
			curl_share_cleanup(handle);
			handle = nullptr;
			throw IOException("Could not configure libcurl connection sharing for AI requests");
		}
	}

	~CurlShareHandle() {
		if (handle) {
			curl_share_cleanup(handle);
		}
	}

	CURLSH *handle;
	CurlShareLocks locks;
};

CURLSH *SharedCurlHandle() {
	static CurlShareHandle share;
	return share.handle;
}

struct CurlEasyHandle {
	CurlEasyHandle() {
		EnsureCurlGlobalInit();
		handle = curl_easy_init();
		if (!handle) {
			throw IOException("Could not initialize libcurl for AI request");
		}
	}

	~CurlEasyHandle() {
		if (handle) {
			curl_easy_cleanup(handle);
		}
	}

	CURL *handle;
};

CURL *ThreadLocalCurlHandle() {
	thread_local CurlEasyHandle handle;
	curl_easy_reset(handle.handle);
	return handle.handle;
}

bool EndsWith(const std::string &value, const std::string &suffix) {
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StartsWith(const std::string &value, const std::string &prefix) {
	return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool HasHttpScheme(const std::string &value) {
	return StartsWith(value, "https://") || StartsWith(value, "http://");
}

std::string AzureOpenAIBaseUrl(std::string value) {
	value = TrimTrailingSlash(std::move(value));
	if (value.empty() || EndsWith(value, "/openai/v1")) {
		return value;
	}
	return value + "/openai/v1";
}

std::string DatabricksBaseUrl(std::string value) {
	value = TrimTrailingSlash(std::move(value));
	if (value.empty()) {
		return value;
	}
	if (!HasHttpScheme(value)) {
		value = "https://" + value;
	}
	if (EndsWith(value, "/serving-endpoints") || EndsWith(value, "/ai-gateway/mlflow/v1") ||
	    EndsWith(value, "/chat/completions")) {
		return value;
	}
	return value + "/serving-endpoints";
}

std::string SnowflakeCortexBaseUrl(std::string value) {
	value = TrimTrailingSlash(std::move(value));
	if (value.empty()) {
		return value;
	}
	if (!HasHttpScheme(value)) {
		value = "https://" + value;
	}
	if (EndsWith(value, "/api/v2/cortex/v1") || EndsWith(value, "/chat/completions")) {
		return value;
	}
	return value + "/api/v2/cortex/v1";
}

std::string SnowflakeAccountBaseUrl(std::string account) {
	account = TrimTrailingSlash(std::move(account));
	if (account.empty()) {
		return account;
	}
	if (HasHttpScheme(account) || account.find(".snowflakecomputing.") != std::string::npos) {
		return SnowflakeCortexBaseUrl(account);
	}
	return SnowflakeCortexBaseUrl(account + ".snowflakecomputing.com");
}

std::string CurrentTimestamp() {
	auto now = std::chrono::system_clock::now();
	auto seconds = std::chrono::system_clock::to_time_t(now);
	std::tm tm_value;
#if defined(_WIN32)
	gmtime_s(&tm_value, &seconds);
#else
	gmtime_r(&seconds, &tm_value);
#endif
	std::ostringstream out;
	out << std::put_time(&tm_value, "%Y-%m-%dT%H:%M:%SZ");
	return out.str();
}

std::string JsonEscape(const std::string &input) {
	std::string out;
	out.reserve(input.size());
	const char *hex = "0123456789abcdef";
	for (auto c : input) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				auto value = static_cast<unsigned char>(c);
				out += "\\u00";
				out.push_back(hex[(value >> 4) & 0xf]);
				out.push_back(hex[value & 0xf]);
			} else {
				out.push_back(c);
			}
		}
	}
	return out;
}

std::string JsonDouble(double input) {
	std::ostringstream out;
	out << std::setprecision(15) << input;
	return out.str();
}

uint64_t StableHash(const std::string &input);

std::string ExtensionUserAgent() {
#ifdef EXT_VERSION_AI
	return std::string("duckdb-ai/") + EXT_VERSION_AI;
#else
	return "duckdb-ai/dev";
#endif
}

bool HasEstimatedCost(double estimated_cost_usd) {
	return estimated_cost_usd >= 0 && std::isfinite(estimated_cost_usd);
}

const std::vector<ModelPrice> &BuiltinModelPrices() {
	static const std::vector<ModelPrice> prices {
	    {"openai", "gpt-5.5", "completion", 5.00, 30.00, "https://developers.openai.com/api/docs/pricing",
	     "standard text token pricing for <272K context length", "2026-07-07"},
	    {"openai", "gpt-5.4", "completion", 2.50, 15.00, "https://developers.openai.com/api/docs/pricing",
	     "standard text token pricing", "2026-06-30"},
	    {"openai", "gpt-5.4-mini", "completion", 0.75, 4.50,
	     "https://developers.openai.com/api/docs/models/gpt-5.4-mini", "standard text token pricing", "2026-06-30"},
	    {"openai", "gpt-5.4-nano", "completion", 0.20, 1.25, "https://developers.openai.com/api/docs/pricing",
	     "standard text token pricing", "2026-06-30"},
	    {"openai", "text-embedding-3-small", "embedding", 0.02, -1,
	     "https://developers.openai.com/api/docs/models/text-embedding-3-small", "embedding input tokens only",
	     "2026-06-30"},
	    {"anthropic", "claude-haiku-4-5", "completion", 1.00, 5.00,
	     "https://platform.claude.com/docs/en/about-claude/pricing", "standard text token pricing", "2026-06-30"},
	    {"anthropic", "claude-3-5-haiku-latest", "completion", 0.80, 4.00,
	     "https://platform.claude.com/docs/en/about-claude/pricing", "legacy Haiku 3.5 pricing", "2026-06-30"},
	    {"anthropic", "claude-sonnet-4-5", "completion", 3.00, 15.00,
	     "https://platform.claude.com/docs/en/about-claude/pricing", "standard text token pricing", "2026-06-30"},
	    {"gemini", "gemini-3.5-flash", "completion", 1.50, 9.00, "https://ai.google.dev/gemini-api/docs/pricing",
	     "standard text token pricing", "2026-07-08"},
	    {"gemini", "gemini-embedding-001", "embedding", 0.15, -1, "https://ai.google.dev/gemini-api/docs/pricing",
	     "standard text embedding input tokens only", "2026-07-07"},
	    {"mistral", "mistral-small-latest", "completion", 0.15, 0.60, "https://mistral.ai/pricing/api/",
	     "standard text token pricing", "2026-07-08"},
	    {"mistral", "mistral-embed", "embedding", 0.10, -1, "https://mistral.ai/pricing/",
	     "embedding input tokens only", "2026-06-30"},
	    {"deepseek", "deepseek-v4-flash", "completion", 0.14, 0.28, "https://api-docs.deepseek.com/quick_start/pricing",
	     "input price uses cache-miss pricing", "2026-07-07"},
	    {"zai", "glm-4.7-flash", "completion", 0.00, 0.00, "https://docs.z.ai/guides/overview/pricing",
	     "standard text token pricing", "2026-06-30"},
	    {"zai", "glm-4.7-flashx", "completion", 0.07, 0.40, "https://docs.z.ai/guides/overview/pricing",
	     "standard text token pricing", "2026-06-30"},
	    {"openrouter", "openai/gpt-4o-mini", "completion", 0.15, 0.60, "https://openrouter.ai/api/v1/models",
	     "live model feed pricing", "2026-06-30"},
	    {"openrouter", "openai/gpt-5.4-mini", "completion", 0.75, 4.50, "https://openrouter.ai/api/v1/models",
	     "live model feed pricing", "2026-06-30"},
	    {"openrouter", "anthropic/claude-haiku-4.5", "completion", 1.00, 5.00, "https://openrouter.ai/api/v1/models",
	     "live model feed pricing", "2026-06-30"},
	    {"openrouter", "z-ai/glm-4.7-flash", "completion", 0.06, 0.40, "https://openrouter.ai/api/v1/models",
	     "live model feed pricing", "2026-06-30"},
	};
	return prices;
}

const ModelPrice *FindBuiltinModelPrice(const std::string &provider, const std::string &model,
                                        const std::string &operation) {
	auto provider_key = LowerAscii(provider);
	auto model_key = LowerAscii(model);
	auto operation_key = LowerAscii(operation);
	for (auto &price : BuiltinModelPrices()) {
		if (LowerAscii(price.provider) == provider_key && LowerAscii(price.model) == model_key &&
		    LowerAscii(price.operation) == operation_key) {
			return &price;
		}
	}
	return nullptr;
}

bool BuiltinModelPricingEnabled(const CompletionOptions &options) {
	if (options.has_use_builtin_model_prices) {
		return options.use_builtin_model_prices;
	}
	auto value = LowerAscii(GetEnv("DUCKDB_AI_USE_BUILTIN_MODEL_PRICES"));
	return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool ResponseCacheEnabled(const CompletionOptions &options) {
	if (options.has_cache) {
		return options.cache;
	}
	return EnvFlagEnabled("DUCKDB_AI_CACHE");
}

bool PromptCacheEnabled(const CompletionOptions &options) {
	if (options.has_prompt_cache) {
		return options.prompt_cache;
	}
	return EnvFlagEnabled("DUCKDB_AI_PROMPT_CACHE");
}

std::string PromptCacheKey(const ProviderConfig &config, const CompletionOptions &options) {
	std::string static_prefix;
	if (!options.system_prompt.empty()) {
		static_prefix += "system:";
		static_prefix += options.system_prompt;
	}
	if (!options.response_schema.empty()) {
		static_prefix += "\nschema:";
		static_prefix += options.response_schema;
	}
	if (static_prefix.empty()) {
		return "";
	}
	auto key_material = config.provider + "\n" + config.model + "\n" + static_prefix;
	return "duckdb-ai-" + std::to_string(StableHash(key_material));
}

void SnapshotEnvironmentOptionsInternal(CompletionOptions &options) {
	if (options.allowed_hosts.empty()) {
		options.allowed_hosts = GetEnv("DUCKDB_AI_ALLOWED_HOSTS");
	}
	if (options.log_endpoint.empty()) {
		options.log_endpoint = GetEnv("DUCKDB_AI_LOG_ENDPOINT");
	}
	if (options.log_format.empty()) {
		options.log_format = GetEnv("DUCKDB_AI_LOG_FORMAT");
	}
	if (options.log_tags.empty()) {
		options.log_tags = GetEnv("DUCKDB_AI_LOG_TAGS");
	}
	if (!options.has_use_builtin_model_prices &&
	    TryReadEnvFlag("DUCKDB_AI_USE_BUILTIN_MODEL_PRICES", options.use_builtin_model_prices)) {
		options.has_use_builtin_model_prices = true;
	}
	if (!options.has_cache && TryReadEnvFlag("DUCKDB_AI_CACHE", options.cache)) {
		options.has_cache = true;
	}
	if (!options.has_prompt_cache && TryReadEnvFlag("DUCKDB_AI_PROMPT_CACHE", options.prompt_cache)) {
		options.has_prompt_cache = true;
	}
	if (!options.has_log_include_text && TryReadEnvFlag("DUCKDB_AI_LOG_INCLUDE_TEXT", options.log_include_text)) {
		options.has_log_include_text = true;
	}
	if (!options.has_log_strict && TryReadEnvFlag("DUCKDB_AI_LOG_STRICT", options.log_strict)) {
		options.has_log_strict = true;
	}
	if (!options.has_timeout_seconds &&
	    TryReadEnvInt64("DUCKDB_AI_TIMEOUT_SECONDS", options.timeout_seconds, 1, 31536000)) {
		options.has_timeout_seconds = true;
	}
	if (!options.has_connect_timeout_seconds &&
	    TryReadEnvInt64("DUCKDB_AI_CONNECT_TIMEOUT_SECONDS", options.connect_timeout_seconds, 1, 31536000)) {
		options.has_connect_timeout_seconds = true;
	}
	if (!options.has_retry_count && TryReadEnvInt64("DUCKDB_AI_RETRY_COUNT", options.retry_count, 0, 10)) {
		options.has_retry_count = true;
	}
	if (!options.has_retry_backoff_ms &&
	    TryReadEnvInt64("DUCKDB_AI_RETRY_BACKOFF_MS", options.retry_backoff_ms, 0, 60000)) {
		options.has_retry_backoff_ms = true;
	}
	if (!options.has_max_concurrent_requests &&
	    TryReadEnvInt64("DUCKDB_AI_MAX_CONCURRENT_REQUESTS", options.max_concurrent_requests, 0,
	                    MAX_PROVIDER_CHUNK_WORKERS)) {
		options.has_max_concurrent_requests = true;
	}
	if (!options.has_min_request_interval_ms &&
	    TryReadEnvInt64("DUCKDB_AI_MIN_REQUEST_INTERVAL_MS", options.min_request_interval_ms, 0, 60000)) {
		options.has_min_request_interval_ms = true;
	}
	if (!options.has_token_limit_per_minute &&
	    TryReadEnvInt64("DUCKDB_AI_TOKEN_LIMIT_PER_MINUTE", options.token_limit_per_minute, 0,
	                    MAX_TOKEN_LIMIT_PER_MINUTE)) {
		options.has_token_limit_per_minute = true;
	}
	if (!options.has_cache_ttl_seconds &&
	    TryReadEnvInt64("DUCKDB_AI_CACHE_TTL_SECONDS", options.cache_ttl_seconds, 0, 31536000)) {
		options.has_cache_ttl_seconds = true;
	}
	if (!options.has_response_cache_max_entries &&
	    TryReadEnvInt64("DUCKDB_AI_CACHE_MAX_ENTRIES", options.response_cache_max_entries, 0, 1000000)) {
		options.has_response_cache_max_entries = true;
	}
	if (!options.has_log_sample_rate && TryReadEnvDouble("DUCKDB_AI_LOG_SAMPLE_RATE", options.log_sample_rate, 0, 1)) {
		options.has_log_sample_rate = true;
	}
}

size_t MaxResponseCacheEntries(const CompletionOptions &options) {
	if (options.has_response_cache_max_entries) {
		return static_cast<size_t>(options.response_cache_max_entries);
	}
	auto configured = GetEnv("DUCKDB_AI_CACHE_MAX_ENTRIES");
	if (configured.empty()) {
		return DEFAULT_MAX_RESPONSE_CACHE_ENTRIES;
	}
	try {
		auto value = std::stoll(configured);
		if (value < 0 || value > 1000000) {
			throw InvalidInputException("DUCKDB_AI_CACHE_MAX_ENTRIES must be between 0 and 1000000");
		}
		return static_cast<size_t>(value);
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_CACHE_MAX_ENTRIES must be an integer between 0 and 1000000");
	}
}

int64_t ResponseCacheTtlSeconds(const CompletionOptions &options) {
	if (options.has_cache_ttl_seconds) {
		return options.cache_ttl_seconds;
	}
	auto configured = GetEnv("DUCKDB_AI_CACHE_TTL_SECONDS");
	if (configured.empty()) {
		return 0;
	}
	try {
		auto value = std::stoll(configured);
		if (value < 0 || value > 31536000) {
			throw InvalidInputException("DUCKDB_AI_CACHE_TTL_SECONDS must be between 0 and 31536000");
		}
		return value;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_CACHE_TTL_SECONDS must be an integer between 0 and 31536000");
	}
}

bool ResponseCacheEntryExpired(const CachedHttpResponse &entry, const CompletionOptions &options) {
	auto ttl_seconds = ResponseCacheTtlSeconds(options);
	if (ttl_seconds <= 0) {
		return false;
	}
	return std::chrono::steady_clock::now() - entry.created_at > std::chrono::seconds(ttl_seconds);
}

std::string ResponseCacheKey(const std::string &operation, const ProviderConfig &config, const std::string &endpoint,
                             const std::string &payload) {
	auto api_key_hash =
	    config.api_key.empty() ? std::string() : std::to_string(std::hash<std::string> {}(config.api_key));
	return operation + "\n" + config.provider + "\n" + config.protocol + "\n" + config.model + "\n" + endpoint + "\n" +
	       api_key_hash + "\n" + payload;
}

void TouchResponseCacheEntry(ProviderRuntimeState &state, CachedHttpResponse &entry, const std::string &cache_key) {
	state.response_cache_order.erase(entry.order_entry);
	entry.order_entry = state.response_cache_order.insert(state.response_cache_order.end(), cache_key);
}

void RemoveCachedResponse(ProviderRuntimeState &state, const std::string &cache_key) {
	std::lock_guard<std::mutex> lock(state.response_cache_mutex);
	auto entry = state.response_cache.find(cache_key);
	if (entry == state.response_cache.end()) {
		return;
	}
	state.response_cache_order.erase(entry->second.order_entry);
	state.response_cache.erase(entry);
}

bool TryGetCachedResponse(ProviderRuntimeState &state, const std::string &cache_key, const CompletionOptions &options,
                          HttpResponse &response) {
	std::lock_guard<std::mutex> lock(state.response_cache_mutex);
	auto entry = state.response_cache.find(cache_key);
	if (entry == state.response_cache.end()) {
		return false;
	}
	if (ResponseCacheEntryExpired(entry->second, options)) {
		state.response_cache_order.erase(entry->second.order_entry);
		state.response_cache.erase(entry);
		return false;
	}
	response = entry->second.response;
	response.elapsed_ms = 0;
	response.retries = 0;
	response.cache_hit = true;
	TouchResponseCacheEntry(state, entry->second, cache_key);
	return true;
}

void StoreCachedResponse(ProviderRuntimeState &state, const std::string &cache_key, const HttpResponse &response,
                         const CompletionOptions &options) {
	auto max_entries = MaxResponseCacheEntries(options);
	if (max_entries == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(state.response_cache_mutex);
	auto entry = state.response_cache.find(cache_key);
	if (entry != state.response_cache.end()) {
		entry->second.response = response;
		entry->second.response.cache_hit = false;
		entry->second.created_at = std::chrono::steady_clock::now();
		TouchResponseCacheEntry(state, entry->second, cache_key);
		return;
	}
	CachedHttpResponse cached;
	cached.response = response;
	cached.response.cache_hit = false;
	cached.created_at = std::chrono::steady_clock::now();
	cached.order_entry = state.response_cache_order.insert(state.response_cache_order.end(), cache_key);
	state.response_cache.emplace(cache_key, std::move(cached));
	while (state.response_cache.size() > max_entries && !state.response_cache_order.empty()) {
		auto oldest = std::move(state.response_cache_order.front());
		state.response_cache_order.pop_front();
		state.response_cache.erase(oldest);
	}
}

std::shared_ptr<PendingHttpResponse> BeginPendingCachedResponse(ProviderRuntimeState &state,
                                                                const std::string &cache_key, bool &owner) {
	std::lock_guard<std::mutex> lock(state.response_cache_mutex);
	auto entry = state.response_cache_pending.find(cache_key);
	if (entry != state.response_cache_pending.end()) {
		owner = false;
		return entry->second;
	}
	auto pending = std::make_shared<PendingHttpResponse>();
	state.response_cache_pending[cache_key] = pending;
	owner = true;
	return pending;
}

bool ClientInterrupted(ClientContext *context);

HttpResponse WaitForPendingCachedResponse(ProviderRuntimeState &state, const std::string &cache_key,
                                          const std::shared_ptr<PendingHttpResponse> &pending,
                                          const CompletionOptions &options) {
	std::unique_lock<std::mutex> lock(state.response_cache_mutex);
	while (!pending->cv.wait_for(lock, std::chrono::milliseconds(100), [&]() { return pending->done; })) {
		if (ClientInterrupted(options.client_context)) {
			throw InterruptException();
		}
	}
	if (!pending->error.empty()) {
		throw IOException("%s", pending->error);
	}
	// Waiters share the owner's provider response, so report them as cache hits to avoid
	// double-counting tokens and cost in usage events.
	auto response = pending->response;
	response.elapsed_ms = 0;
	response.retries = 0;
	response.cache_hit = true;
	return response;
}

void FinishPendingCachedResponse(ProviderRuntimeState &state, const std::string &cache_key,
                                 const std::shared_ptr<PendingHttpResponse> &pending, const HttpResponse &response) {
	std::lock_guard<std::mutex> lock(state.response_cache_mutex);
	pending->response = response;
	pending->error.clear();
	pending->done = true;
	state.response_cache_pending.erase(cache_key);
	pending->cv.notify_all();
}

void FailPendingCachedResponse(ProviderRuntimeState &state, const std::string &cache_key,
                               const std::shared_ptr<PendingHttpResponse> &pending, const std::string &error) {
	std::lock_guard<std::mutex> lock(state.response_cache_mutex);
	pending->error = error;
	pending->done = true;
	state.response_cache_pending.erase(cache_key);
	pending->cv.notify_all();
}

enum class JsonValueType { NULL_VALUE, BOOLEAN, NUMBER, STRING, ARRAY, OBJECT };

struct JsonValue {
	JsonValueType type = JsonValueType::NULL_VALUE;
	bool boolean_value = false;
	double number_value = 0;
	bool number_is_integer = false;
	std::string string_value;
	std::vector<JsonValue> array_value;
	std::map<std::string, JsonValue> object_value;
};

using YyjsonDocPtr = std::unique_ptr<duckdb_yyjson::yyjson_doc, decltype(&duckdb_yyjson::yyjson_doc_free)>;

std::string YyjsonParseError(const duckdb_yyjson::yyjson_read_err &read_error) {
	if (read_error.code == duckdb_yyjson::YYJSON_READ_SUCCESS) {
		return "";
	}
	return "malformed JSON at byte " + std::to_string(read_error.pos) + ": " + read_error.msg;
}

YyjsonDocPtr ReadYyjsonDocument(const std::string &input, std::string &error) {
	duckdb_yyjson::yyjson_read_err read_error;
	auto doc = duckdb_yyjson::yyjson_read_opts(const_cast<char *>(input.data()), input.size(),
	                                           duckdb_yyjson::YYJSON_READ_NOFLAG, nullptr, &read_error);
	if (!doc) {
		error = YyjsonParseError(read_error);
		return YyjsonDocPtr(nullptr, duckdb_yyjson::yyjson_doc_free);
	}
	return YyjsonDocPtr(doc, duckdb_yyjson::yyjson_doc_free);
}

std::string YyjsonString(duckdb_yyjson::yyjson_val *value) {
	return std::string(duckdb_yyjson::yyjson_get_str(value), duckdb_yyjson::yyjson_get_len(value));
}

duckdb_yyjson::yyjson_val *YyjsonObjectGet(duckdb_yyjson::yyjson_val *object, const char *key) {
	if (!object || !duckdb_yyjson::yyjson_is_obj(object)) {
		return nullptr;
	}
	return duckdb_yyjson::yyjson_obj_get(object, key);
}

duckdb_yyjson::yyjson_val *YyjsonArrayGet(duckdb_yyjson::yyjson_val *array, idx_t index) {
	if (!array || !duckdb_yyjson::yyjson_is_arr(array)) {
		return nullptr;
	}
	return duckdb_yyjson::yyjson_arr_get(array, static_cast<size_t>(index));
}

bool YyjsonDirectString(duckdb_yyjson::yyjson_val *object, const char *key, std::string &result) {
	auto value = YyjsonObjectGet(object, key);
	if (!value || !duckdb_yyjson::yyjson_is_str(value)) {
		return false;
	}
	result = YyjsonString(value);
	return true;
}

bool YyjsonDirectInteger(duckdb_yyjson::yyjson_val *object, const char *key, int64_t &result) {
	auto value = YyjsonObjectGet(object, key);
	if (!value || !duckdb_yyjson::yyjson_is_int(value)) {
		return false;
	}
	if (duckdb_yyjson::yyjson_is_uint(value)) {
		auto unsigned_value = duckdb_yyjson::yyjson_get_uint(value);
		if (unsigned_value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			return false;
		}
		result = static_cast<int64_t>(unsigned_value);
	} else {
		result = duckdb_yyjson::yyjson_get_sint(value);
	}
	return true;
}

int64_t YyjsonIntegerOrMissing(duckdb_yyjson::yyjson_val *object, const char *key) {
	int64_t result = -1;
	YyjsonDirectInteger(object, key, result);
	return result;
}

bool YyjsonNumericArray(duckdb_yyjson::yyjson_val *value, std::vector<double> &result);

bool YyjsonDirectNumberArray(duckdb_yyjson::yyjson_val *object, const char *key, std::vector<double> &result) {
	auto value = YyjsonObjectGet(object, key);
	return value && YyjsonNumericArray(value, result);
}

bool ConvertYyjsonValue(duckdb_yyjson::yyjson_val *source, JsonValue &target, std::string &error) {
	if (!source || duckdb_yyjson::yyjson_is_null(source)) {
		target.type = JsonValueType::NULL_VALUE;
		return true;
	}
	if (duckdb_yyjson::yyjson_is_bool(source)) {
		target.type = JsonValueType::BOOLEAN;
		target.boolean_value = duckdb_yyjson::yyjson_get_bool(source);
		return true;
	}
	if (duckdb_yyjson::yyjson_is_num(source)) {
		target.type = JsonValueType::NUMBER;
		target.number_value = duckdb_yyjson::yyjson_get_num(source);
		target.number_is_integer = duckdb_yyjson::yyjson_is_int(source);
		return true;
	}
	if (duckdb_yyjson::yyjson_is_str(source)) {
		target.type = JsonValueType::STRING;
		target.string_value = YyjsonString(source);
		return true;
	}
	if (duckdb_yyjson::yyjson_is_arr(source)) {
		target.type = JsonValueType::ARRAY;
		duckdb_yyjson::yyjson_val *entry;
		size_t index;
		size_t max;
		yyjson_arr_foreach(source, index, max, entry) {
			JsonValue child;
			if (!ConvertYyjsonValue(entry, child, error)) {
				return false;
			}
			target.array_value.push_back(std::move(child));
		}
		return true;
	}
	if (duckdb_yyjson::yyjson_is_obj(source)) {
		target.type = JsonValueType::OBJECT;
		duckdb_yyjson::yyjson_val *key;
		duckdb_yyjson::yyjson_val *value;
		size_t index;
		size_t max;
		yyjson_obj_foreach(source, index, max, key, value) {
			JsonValue child;
			if (!ConvertYyjsonValue(value, child, error)) {
				return false;
			}
			target.object_value[YyjsonString(key)] = std::move(child);
		}
		return true;
	}
	error = "unsupported JSON value type";
	return false;
}

bool FindYyjsonStringField(duckdb_yyjson::yyjson_val *value, const std::string &field, std::string &result) {
	if (!value) {
		return false;
	}
	if (duckdb_yyjson::yyjson_is_obj(value)) {
		duckdb_yyjson::yyjson_val *key;
		duckdb_yyjson::yyjson_val *child;
		size_t index;
		size_t max;
		yyjson_obj_foreach(value, index, max, key, child) {
			if (YyjsonString(key) == field && duckdb_yyjson::yyjson_is_str(child)) {
				result = YyjsonString(child);
				return true;
			}
			if (FindYyjsonStringField(child, field, result)) {
				return true;
			}
		}
		return false;
	}
	if (duckdb_yyjson::yyjson_is_arr(value)) {
		duckdb_yyjson::yyjson_val *child;
		size_t index;
		size_t max;
		yyjson_arr_foreach(value, index, max, child) {
			if (FindYyjsonStringField(child, field, result)) {
				return true;
			}
		}
	}
	return false;
}

bool FindYyjsonIntegerField(duckdb_yyjson::yyjson_val *value, const std::string &field, int64_t &result) {
	if (!value) {
		return false;
	}
	if (duckdb_yyjson::yyjson_is_obj(value)) {
		duckdb_yyjson::yyjson_val *key;
		duckdb_yyjson::yyjson_val *child;
		size_t index;
		size_t max;
		yyjson_obj_foreach(value, index, max, key, child) {
			if (YyjsonString(key) == field && duckdb_yyjson::yyjson_is_int(child)) {
				if (duckdb_yyjson::yyjson_is_uint(child)) {
					auto unsigned_value = duckdb_yyjson::yyjson_get_uint(child);
					if (unsigned_value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
						return false;
					}
					result = static_cast<int64_t>(unsigned_value);
				} else {
					result = duckdb_yyjson::yyjson_get_sint(child);
				}
				return true;
			}
			if (FindYyjsonIntegerField(child, field, result)) {
				return true;
			}
		}
		return false;
	}
	if (duckdb_yyjson::yyjson_is_arr(value)) {
		duckdb_yyjson::yyjson_val *child;
		size_t index;
		size_t max;
		yyjson_arr_foreach(value, index, max, child) {
			if (FindYyjsonIntegerField(child, field, result)) {
				return true;
			}
		}
	}
	return false;
}

bool YyjsonNumericArray(duckdb_yyjson::yyjson_val *value, std::vector<double> &result) {
	if (!duckdb_yyjson::yyjson_is_arr(value)) {
		return false;
	}
	duckdb_yyjson::yyjson_val *child;
	size_t index;
	size_t max;
	yyjson_arr_foreach(value, index, max, child) {
		if (!duckdb_yyjson::yyjson_is_num(child)) {
			return false;
		}
		result.push_back(duckdb_yyjson::yyjson_get_num(child));
	}
	return true;
}

bool FindFirstYyjsonNumberArray(duckdb_yyjson::yyjson_val *value, std::vector<double> &result) {
	if (!value) {
		return false;
	}
	if (duckdb_yyjson::yyjson_is_arr(value)) {
		std::vector<double> candidate;
		if (YyjsonNumericArray(value, candidate)) {
			result = std::move(candidate);
			return true;
		}
		duckdb_yyjson::yyjson_val *child;
		size_t index;
		size_t max;
		yyjson_arr_foreach(value, index, max, child) {
			if (FindFirstYyjsonNumberArray(child, result)) {
				return true;
			}
		}
		return false;
	}
	if (duckdb_yyjson::yyjson_is_obj(value)) {
		duckdb_yyjson::yyjson_val *key;
		duckdb_yyjson::yyjson_val *child;
		size_t index;
		size_t max;
		yyjson_obj_foreach(value, index, max, key, child) {
			if (FindFirstYyjsonNumberArray(child, result)) {
				return true;
			}
		}
	}
	return false;
}

bool FindYyjsonNumberArrayField(duckdb_yyjson::yyjson_val *value, const std::string &field,
                                std::vector<double> &result) {
	if (!value) {
		return false;
	}
	if (duckdb_yyjson::yyjson_is_obj(value)) {
		duckdb_yyjson::yyjson_val *key;
		duckdb_yyjson::yyjson_val *child;
		size_t index;
		size_t max;
		yyjson_obj_foreach(value, index, max, key, child) {
			if (YyjsonString(key) == field && FindFirstYyjsonNumberArray(child, result)) {
				return true;
			}
			if (FindYyjsonNumberArrayField(child, field, result)) {
				return true;
			}
		}
		return false;
	}
	if (duckdb_yyjson::yyjson_is_arr(value)) {
		duckdb_yyjson::yyjson_val *child;
		size_t index;
		size_t max;
		yyjson_arr_foreach(value, index, max, child) {
			if (FindYyjsonNumberArrayField(child, field, result)) {
				return true;
			}
		}
	}
	return false;
}

bool ParseJsonValueDocument(const std::string &input, JsonValue &value, std::string &error) {
	auto doc = ReadYyjsonDocument(input, error);
	if (!doc) {
		return false;
	}
	auto root = duckdb_yyjson::yyjson_doc_get_root(doc.get());
	if (!root) {
		error = "empty JSON document";
		return false;
	}
	return ConvertYyjsonValue(root, value, error);
}

const JsonValue *ObjectField(const JsonValue &value, const std::string &field) {
	if (value.type != JsonValueType::OBJECT) {
		return nullptr;
	}
	auto entry = value.object_value.find(field);
	return entry == value.object_value.end() ? nullptr : &entry->second;
}

bool SchemaNumberField(const JsonValue &schema, const std::string &field, double &value) {
	auto field_value = ObjectField(schema, field);
	if (!field_value || field_value->type != JsonValueType::NUMBER) {
		return false;
	}
	value = field_value->number_value;
	return true;
}

bool SchemaNonNegativeIntegerField(const JsonValue &schema, const std::string &field, int64_t &value) {
	double number_value;
	if (!SchemaNumberField(schema, field, number_value)) {
		return false;
	}
	if (number_value < 0 || std::floor(number_value) != number_value) {
		return false;
	}
	value = static_cast<int64_t>(number_value);
	return true;
}

bool IsJsonInteger(const JsonValue &value) {
	if (value.type != JsonValueType::NUMBER) {
		return false;
	}
	return value.number_is_integer || std::floor(value.number_value) == value.number_value;
}

std::string JsonTypeName(const JsonValue &value) {
	switch (value.type) {
	case JsonValueType::NULL_VALUE:
		return "null";
	case JsonValueType::BOOLEAN:
		return "boolean";
	case JsonValueType::NUMBER:
		return IsJsonInteger(value) ? "integer" : "number";
	case JsonValueType::STRING:
		return "string";
	case JsonValueType::ARRAY:
		return "array";
	case JsonValueType::OBJECT:
		return "object";
	}
	return "unknown";
}

bool JsonValueEquals(const JsonValue &left, const JsonValue &right) {
	if (left.type == JsonValueType::NUMBER && right.type == JsonValueType::NUMBER) {
		return left.number_value == right.number_value;
	}
	if (left.type != right.type) {
		return false;
	}
	switch (left.type) {
	case JsonValueType::NULL_VALUE:
		return true;
	case JsonValueType::BOOLEAN:
		return left.boolean_value == right.boolean_value;
	case JsonValueType::STRING:
		return left.string_value == right.string_value;
	case JsonValueType::ARRAY:
		if (left.array_value.size() != right.array_value.size()) {
			return false;
		}
		for (idx_t i = 0; i < left.array_value.size(); i++) {
			if (!JsonValueEquals(left.array_value[i], right.array_value[i])) {
				return false;
			}
		}
		return true;
	case JsonValueType::OBJECT:
		if (left.object_value.size() != right.object_value.size()) {
			return false;
		}
		for (auto &entry : left.object_value) {
			auto right_entry = right.object_value.find(entry.first);
			if (right_entry == right.object_value.end() || !JsonValueEquals(entry.second, right_entry->second)) {
				return false;
			}
		}
		return true;
	case JsonValueType::NUMBER:
		break;
	}
	return false;
}

std::string JsonValueToJson(const JsonValue &value) {
	switch (value.type) {
	case JsonValueType::NULL_VALUE:
		return "null";
	case JsonValueType::BOOLEAN:
		return value.boolean_value ? "true" : "false";
	case JsonValueType::NUMBER:
		return JsonDouble(value.number_value);
	case JsonValueType::STRING:
		return "\"" + JsonEscape(value.string_value) + "\"";
	case JsonValueType::ARRAY: {
		std::string output = "[";
		for (idx_t i = 0; i < value.array_value.size(); i++) {
			if (i > 0) {
				output += ",";
			}
			output += JsonValueToJson(value.array_value[i]);
		}
		output += "]";
		return output;
	}
	case JsonValueType::OBJECT: {
		std::string output = "{";
		bool first = true;
		for (auto &entry : value.object_value) {
			if (!first) {
				output += ",";
			}
			first = false;
			output += "\"" + JsonEscape(entry.first) + "\":" + JsonValueToJson(entry.second);
		}
		output += "}";
		return output;
	}
	}
	return "null";
}

bool SchemaTypeAllows(const JsonValue &type_schema, const JsonValue &value) {
	auto value_type = JsonTypeName(value);
	auto allows_type = [&](const std::string &type_name) {
		return type_name == value_type || (type_name == "number" && value.type == JsonValueType::NUMBER) ||
		       (type_name == "integer" && IsJsonInteger(value));
	};
	if (type_schema.type == JsonValueType::STRING) {
		return allows_type(type_schema.string_value);
	}
	if (type_schema.type == JsonValueType::ARRAY) {
		for (auto &entry : type_schema.array_value) {
			if (entry.type == JsonValueType::STRING && allows_type(entry.string_value)) {
				return true;
			}
		}
	}
	return true;
}

std::string JsonPathChild(const std::string &path, const std::string &field) {
	return path + "." + field;
}

std::string JsonPathIndex(const std::string &path, idx_t index) {
	return path + "[" + std::to_string(index) + "]";
}

std::string SchemaTypeName(const JsonValue &schema) {
	auto type_schema = ObjectField(schema, "type");
	if (type_schema && type_schema->type == JsonValueType::STRING) {
		return type_schema->string_value;
	}
	if (type_schema && type_schema->type == JsonValueType::ARRAY) {
		for (auto &entry : type_schema->array_value) {
			if (entry.type == JsonValueType::STRING && entry.string_value != "null") {
				return entry.string_value;
			}
		}
	}
	if (ObjectField(schema, "properties")) {
		return "object";
	}
	if (ObjectField(schema, "items")) {
		return "array";
	}
	return "json";
}

void ExtractSchemaPropertyList(const JsonValue &schema, std::vector<JsonSchemaProperty> &properties) {
	if (schema.type != JsonValueType::OBJECT) {
		return;
	}
	auto properties_schema = ObjectField(schema, "properties");
	if (!properties_schema || properties_schema->type != JsonValueType::OBJECT) {
		return;
	}
	auto required_schema = ObjectField(schema, "required");
	for (auto &entry : properties_schema->object_value) {
		JsonSchemaProperty property;
		property.name = entry.first;
		property.type = entry.second.type == JsonValueType::OBJECT ? SchemaTypeName(entry.second) : "json";
		property.required = false;
		if (required_schema && required_schema->type == JsonValueType::ARRAY) {
			for (auto &required : required_schema->array_value) {
				if (required.type == JsonValueType::STRING && required.string_value == property.name) {
					property.required = true;
					break;
				}
			}
		}
		if (property.type == "object" && entry.second.type == JsonValueType::OBJECT) {
			ExtractSchemaPropertyList(entry.second, property.children);
		}
		if (property.type == "array" && entry.second.type == JsonValueType::OBJECT) {
			auto items_schema = ObjectField(entry.second, "items");
			if (items_schema && items_schema->type == JsonValueType::OBJECT) {
				property.item_type = SchemaTypeName(*items_schema);
				if (property.item_type == "object") {
					ExtractSchemaPropertyList(*items_schema, property.item_children);
				}
			}
		}
		properties.push_back(std::move(property));
	}
}

bool RegexMatches(const std::string &value, const std::string &pattern, const std::string &path, std::string &error) {
	static std::mutex regex_cache_mutex;
	static std::unordered_map<std::string, std::shared_ptr<std::regex>> regex_cache;
	constexpr size_t max_regex_cache_entries = 256;
	try {
		std::shared_ptr<std::regex> regex;
		{
			std::lock_guard<std::mutex> lock(regex_cache_mutex);
			auto entry = regex_cache.find(pattern);
			if (entry == regex_cache.end()) {
				if (regex_cache.size() >= max_regex_cache_entries) {
					regex_cache.clear();
				}
				regex = std::make_shared<std::regex>(pattern);
				regex_cache.emplace(pattern, regex);
			} else {
				regex = entry->second;
			}
		}
		return std::regex_search(value, *regex);
	} catch (std::regex_error &ex) {
		error = path + " schema pattern is invalid: " + ex.what();
		return false;
	}
}

bool ValidateJsonValueAgainstSchema(const JsonValue &value, const JsonValue &schema, const std::string &path,
                                    std::string &error) {
	if (schema.type != JsonValueType::OBJECT) {
		return true;
	}
	auto all_of_schema = ObjectField(schema, "allOf");
	if (all_of_schema && all_of_schema->type == JsonValueType::ARRAY) {
		for (idx_t i = 0; i < all_of_schema->array_value.size(); i++) {
			std::string child_error;
			if (!ValidateJsonValueAgainstSchema(value, all_of_schema->array_value[i], path, child_error)) {
				error = path + " failed allOf schema " + std::to_string(i) + ": " + child_error;
				return false;
			}
		}
	}
	auto any_of_schema = ObjectField(schema, "anyOf");
	if (any_of_schema && any_of_schema->type == JsonValueType::ARRAY) {
		auto matched = false;
		for (auto &candidate : any_of_schema->array_value) {
			std::string child_error;
			if (ValidateJsonValueAgainstSchema(value, candidate, path, child_error)) {
				matched = true;
				break;
			}
		}
		if (!matched) {
			error = path + " does not match any schema in anyOf";
			return false;
		}
	}
	auto one_of_schema = ObjectField(schema, "oneOf");
	if (one_of_schema && one_of_schema->type == JsonValueType::ARRAY) {
		idx_t matches = 0;
		for (auto &candidate : one_of_schema->array_value) {
			std::string child_error;
			if (ValidateJsonValueAgainstSchema(value, candidate, path, child_error)) {
				matches++;
			}
		}
		if (matches != 1) {
			error = path + " matched " + std::to_string(matches) + " schemas in oneOf";
			return false;
		}
	}
	auto not_schema = ObjectField(schema, "not");
	if (not_schema && not_schema->type == JsonValueType::OBJECT) {
		std::string child_error;
		if (ValidateJsonValueAgainstSchema(value, *not_schema, path, child_error)) {
			error = path + " matched schema in not";
			return false;
		}
	}
	auto const_schema = ObjectField(schema, "const");
	if (const_schema && !JsonValueEquals(value, *const_schema)) {
		error = path + " does not match schema const";
		return false;
	}
	auto enum_schema = ObjectField(schema, "enum");
	if (enum_schema && enum_schema->type == JsonValueType::ARRAY) {
		auto matched = false;
		for (auto &allowed : enum_schema->array_value) {
			if (JsonValueEquals(value, allowed)) {
				matched = true;
				break;
			}
		}
		if (!matched) {
			error = path + " does not match any schema enum value";
			return false;
		}
	}
	auto type_schema = ObjectField(schema, "type");
	if (type_schema && !SchemaTypeAllows(*type_schema, value)) {
		error = path + " expected " +
		        (type_schema->type == JsonValueType::STRING ? type_schema->string_value : "one of schema type values") +
		        " but got " + JsonTypeName(value);
		return false;
	}
	if (value.type == JsonValueType::NUMBER) {
		double numeric_bound;
		auto exclusive_minimum = ObjectField(schema, "exclusiveMinimum");
		if (exclusive_minimum && exclusive_minimum->type == JsonValueType::NUMBER &&
		    value.number_value <= exclusive_minimum->number_value) {
			error = path + " must be greater than " + JsonDouble(exclusive_minimum->number_value);
			return false;
		}
		if (exclusive_minimum && exclusive_minimum->type == JsonValueType::BOOLEAN &&
		    exclusive_minimum->boolean_value && SchemaNumberField(schema, "minimum", numeric_bound) &&
		    value.number_value <= numeric_bound) {
			error = path + " must be greater than " + JsonDouble(numeric_bound);
			return false;
		}
		if (SchemaNumberField(schema, "minimum", numeric_bound) && value.number_value < numeric_bound) {
			error = path + " must be greater than or equal to " + JsonDouble(numeric_bound);
			return false;
		}
		auto exclusive_maximum = ObjectField(schema, "exclusiveMaximum");
		if (exclusive_maximum && exclusive_maximum->type == JsonValueType::NUMBER &&
		    value.number_value >= exclusive_maximum->number_value) {
			error = path + " must be less than " + JsonDouble(exclusive_maximum->number_value);
			return false;
		}
		if (exclusive_maximum && exclusive_maximum->type == JsonValueType::BOOLEAN &&
		    exclusive_maximum->boolean_value && SchemaNumberField(schema, "maximum", numeric_bound) &&
		    value.number_value >= numeric_bound) {
			error = path + " must be less than " + JsonDouble(numeric_bound);
			return false;
		}
		if (SchemaNumberField(schema, "maximum", numeric_bound) && value.number_value > numeric_bound) {
			error = path + " must be less than or equal to " + JsonDouble(numeric_bound);
			return false;
		}
		if (SchemaNumberField(schema, "multipleOf", numeric_bound) && numeric_bound > 0) {
			auto remainder = std::fmod(std::fabs(value.number_value), numeric_bound);
			auto tolerance = std::max(1e-12, std::fabs(numeric_bound) * 1e-12);
			if (remainder > tolerance && std::fabs(remainder - numeric_bound) > tolerance) {
				error = path + " must be a multiple of " + JsonDouble(numeric_bound);
				return false;
			}
		}
	}
	if (value.type == JsonValueType::STRING) {
		int64_t string_limit;
		if (SchemaNonNegativeIntegerField(schema, "minLength", string_limit) &&
		    value.string_value.size() < static_cast<idx_t>(string_limit)) {
			error = path + " length must be at least " + std::to_string(string_limit);
			return false;
		}
		if (SchemaNonNegativeIntegerField(schema, "maxLength", string_limit) &&
		    value.string_value.size() > static_cast<idx_t>(string_limit)) {
			error = path + " length must be at most " + std::to_string(string_limit);
			return false;
		}
		auto pattern_schema = ObjectField(schema, "pattern");
		if (pattern_schema && pattern_schema->type == JsonValueType::STRING) {
			std::string pattern_error;
			if (!RegexMatches(value.string_value, pattern_schema->string_value, path, pattern_error)) {
				error = pattern_error.empty() ? path + " does not match schema pattern" : pattern_error;
				return false;
			}
		}
	}
	if (value.type == JsonValueType::OBJECT) {
		auto properties_schema = ObjectField(schema, "properties");
		auto pattern_properties_schema = ObjectField(schema, "patternProperties");
		auto required_schema = ObjectField(schema, "required");
		int64_t property_limit;
		if (SchemaNonNegativeIntegerField(schema, "minProperties", property_limit) &&
		    value.object_value.size() < static_cast<idx_t>(property_limit)) {
			error = path + " must have at least " + std::to_string(property_limit) + " properties";
			return false;
		}
		if (SchemaNonNegativeIntegerField(schema, "maxProperties", property_limit) &&
		    value.object_value.size() > static_cast<idx_t>(property_limit)) {
			error = path + " must have at most " + std::to_string(property_limit) + " properties";
			return false;
		}
		if (required_schema && required_schema->type == JsonValueType::ARRAY) {
			for (auto &required : required_schema->array_value) {
				if (required.type != JsonValueType::STRING) {
					continue;
				}
				if (value.object_value.find(required.string_value) == value.object_value.end()) {
					error = JsonPathChild(path, required.string_value) + " is required";
					return false;
				}
			}
		}
		auto property_names_schema = ObjectField(schema, "propertyNames");
		if (property_names_schema && property_names_schema->type == JsonValueType::OBJECT) {
			for (auto &property : value.object_value) {
				JsonValue property_name;
				property_name.type = JsonValueType::STRING;
				property_name.string_value = property.first;
				if (!ValidateJsonValueAgainstSchema(property_name, *property_names_schema,
				                                    JsonPathChild(path, property.first), error)) {
					return false;
				}
			}
		}
		if (properties_schema && properties_schema->type == JsonValueType::OBJECT) {
			for (auto &property_schema : properties_schema->object_value) {
				auto property = value.object_value.find(property_schema.first);
				if (property != value.object_value.end() &&
				    !ValidateJsonValueAgainstSchema(property->second, property_schema.second,
				                                    JsonPathChild(path, property_schema.first), error)) {
					return false;
				}
			}
		}
		if (pattern_properties_schema && pattern_properties_schema->type == JsonValueType::OBJECT) {
			for (auto &property : value.object_value) {
				for (auto &pattern_schema : pattern_properties_schema->object_value) {
					std::string pattern_error;
					if (!RegexMatches(property.first, pattern_schema.first, JsonPathChild(path, property.first),
					                  pattern_error)) {
						if (!pattern_error.empty()) {
							error = pattern_error;
							return false;
						}
						continue;
					}
					if (!ValidateJsonValueAgainstSchema(property.second, pattern_schema.second,
					                                    JsonPathChild(path, property.first), error)) {
						return false;
					}
				}
			}
		}
		auto dependent_required_schema = ObjectField(schema, "dependentRequired");
		if (dependent_required_schema && dependent_required_schema->type == JsonValueType::OBJECT) {
			for (auto &dependency : dependent_required_schema->object_value) {
				if (value.object_value.find(dependency.first) == value.object_value.end() ||
				    dependency.second.type != JsonValueType::ARRAY) {
					continue;
				}
				for (auto &required : dependency.second.array_value) {
					if (required.type != JsonValueType::STRING) {
						continue;
					}
					if (value.object_value.find(required.string_value) == value.object_value.end()) {
						error = JsonPathChild(path, required.string_value) + " is required by dependentRequired";
						return false;
					}
				}
			}
		}
		auto additional_schema = ObjectField(schema, "additionalProperties");
		if (additional_schema) {
			for (auto &property : value.object_value) {
				auto declared =
				    properties_schema && properties_schema->type == JsonValueType::OBJECT &&
				    properties_schema->object_value.find(property.first) != properties_schema->object_value.end();
				if (!declared && pattern_properties_schema &&
				    pattern_properties_schema->type == JsonValueType::OBJECT) {
					for (auto &pattern_schema : pattern_properties_schema->object_value) {
						std::string pattern_error;
						if (!RegexMatches(property.first, pattern_schema.first, JsonPathChild(path, property.first),
						                  pattern_error)) {
							if (!pattern_error.empty()) {
								error = pattern_error;
								return false;
							}
							continue;
						}
						declared = true;
						break;
					}
				}
				if (declared) {
					continue;
				}
				if (additional_schema->type == JsonValueType::BOOLEAN && !additional_schema->boolean_value) {
					error = JsonPathChild(path, property.first) + " is not allowed by additionalProperties";
					return false;
				}
				if (additional_schema->type == JsonValueType::OBJECT &&
				    !ValidateJsonValueAgainstSchema(property.second, *additional_schema,
				                                    JsonPathChild(path, property.first), error)) {
					return false;
				}
			}
		}
	}
	if (value.type == JsonValueType::ARRAY) {
		int64_t item_limit;
		if (SchemaNonNegativeIntegerField(schema, "minItems", item_limit) &&
		    value.array_value.size() < static_cast<idx_t>(item_limit)) {
			error = path + " must have at least " + std::to_string(item_limit) + " items";
			return false;
		}
		if (SchemaNonNegativeIntegerField(schema, "maxItems", item_limit) &&
		    value.array_value.size() > static_cast<idx_t>(item_limit)) {
			error = path + " must have at most " + std::to_string(item_limit) + " items";
			return false;
		}
		auto unique_items_schema = ObjectField(schema, "uniqueItems");
		if (unique_items_schema && unique_items_schema->type == JsonValueType::BOOLEAN &&
		    unique_items_schema->boolean_value) {
			for (idx_t i = 0; i < value.array_value.size(); i++) {
				for (idx_t j = i + 1; j < value.array_value.size(); j++) {
					if (JsonValueEquals(value.array_value[i], value.array_value[j])) {
						error = JsonPathIndex(path, j) + " duplicates an earlier item";
						return false;
					}
				}
			}
		}
		auto items_schema = ObjectField(schema, "items");
		if (items_schema && items_schema->type == JsonValueType::OBJECT) {
			for (idx_t i = 0; i < value.array_value.size(); i++) {
				if (!ValidateJsonValueAgainstSchema(value.array_value[i], *items_schema, JsonPathIndex(path, i),
				                                    error)) {
					return false;
				}
			}
		}
		auto contains_schema = ObjectField(schema, "contains");
		if (contains_schema && contains_schema->type == JsonValueType::OBJECT) {
			idx_t matches = 0;
			for (auto &item : value.array_value) {
				std::string child_error;
				if (ValidateJsonValueAgainstSchema(item, *contains_schema, path, child_error)) {
					matches++;
				}
			}
			int64_t contains_limit = 1;
			SchemaNonNegativeIntegerField(schema, "minContains", contains_limit);
			if (matches < static_cast<idx_t>(contains_limit)) {
				error = path + " must contain at least " + std::to_string(contains_limit) + " matching items";
				return false;
			}
			if (SchemaNonNegativeIntegerField(schema, "maxContains", contains_limit) &&
			    matches > static_cast<idx_t>(contains_limit)) {
				error = path + " must contain at most " + std::to_string(contains_limit) + " matching items";
				return false;
			}
		}
	}
	return true;
}

bool FindJsonStringValue(const std::string &body, const std::string &key, std::string &value) {
	std::string error;
	auto doc = ReadYyjsonDocument(body, error);
	if (!doc) {
		return false;
	}
	return FindYyjsonStringField(duckdb_yyjson::yyjson_doc_get_root(doc.get()), key, value);
}

int64_t FindJsonIntegerValue(const std::string &body, const std::string &key) {
	std::string error;
	auto doc = ReadYyjsonDocument(body, error);
	if (!doc) {
		return -1;
	}
	int64_t value = -1;
	if (FindYyjsonIntegerField(duckdb_yyjson::yyjson_doc_get_root(doc.get()), key, value)) {
		return value;
	}
	return -1;
}

size_t WriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
	auto response = reinterpret_cast<std::string *>(userdata);
	response->append(ptr, size * nmemb);
	return size * nmemb;
}

bool ClientInterrupted(ClientContext *context) {
	return context && context->IsInterrupted();
}

struct CurlHeaderCapture {
	long retry_after_ms = -1;
};

long ParseRetryAfterMs(const std::string &value) {
	auto trimmed = TrimAscii(value);
	if (trimmed.empty()) {
		return -1;
	}
	try {
		size_t parsed = 0;
		auto seconds = std::stol(trimmed, &parsed);
		if (parsed == trimmed.size() && seconds >= 0) {
			return seconds > 3600 ? 3600000 : seconds * 1000;
		}
	} catch (...) {
	}
	auto retry_time = curl_getdate(trimmed.c_str(), nullptr);
	if (retry_time < 0) {
		return -1;
	}
	auto now = std::time(nullptr);
	if (retry_time <= now) {
		return 0;
	}
	auto seconds = retry_time - now;
	return seconds > 3600 ? 3600000 : static_cast<long>(seconds * 1000);
}

size_t HeaderCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
	auto total = size * nitems;
	auto capture = reinterpret_cast<CurlHeaderCapture *>(userdata);
	std::string header(buffer, total);
	auto lower_header = LowerAscii(header);
	const std::string prefix = "retry-after:";
	if (lower_header.compare(0, prefix.size(), prefix) == 0) {
		capture->retry_after_ms = ParseRetryAfterMs(header.substr(prefix.size()));
	}
	return total;
}

struct CurlProgressState {
	ClientContext *client_context;
};

int CurlProgressCallback(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto state = reinterpret_cast<CurlProgressState *>(clientp);
	return ClientInterrupted(state ? state->client_context : nullptr) ? 1 : 0;
}

long TimeoutSeconds() {
	auto configured = GetEnv("DUCKDB_AI_TIMEOUT_SECONDS");
	if (configured.empty()) {
		return 120;
	}
	try {
		auto timeout = std::stol(configured);
		return timeout > 0 ? timeout : 120;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_TIMEOUT_SECONDS must be a positive integer");
	}
}

long TimeoutSeconds(const CompletionOptions &options) {
	if (!options.has_timeout_seconds) {
		return TimeoutSeconds();
	}
	if (options.timeout_seconds <= 0) {
		throw InvalidInputException("AI timeout must be a positive integer");
	}
	return static_cast<long>(options.timeout_seconds);
}

long ConnectTimeoutSeconds(long timeout_seconds) {
	auto configured = GetEnv("DUCKDB_AI_CONNECT_TIMEOUT_SECONDS");
	if (configured.empty()) {
		return std::max<long>(1, std::min<long>(10, timeout_seconds));
	}
	try {
		auto timeout = std::stol(configured);
		if (timeout <= 0 || timeout > timeout_seconds) {
			throw InvalidInputException("DUCKDB_AI_CONNECT_TIMEOUT_SECONDS must be between 1 and the total timeout");
		}
		return timeout;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_CONNECT_TIMEOUT_SECONDS must be a positive integer");
	}
}

long ConnectTimeoutSeconds(const CompletionOptions &options, long timeout_seconds) {
	if (!options.has_connect_timeout_seconds) {
		return ConnectTimeoutSeconds(timeout_seconds);
	}
	auto connect_timeout_seconds = static_cast<long>(options.connect_timeout_seconds);
	if (connect_timeout_seconds <= 0 || connect_timeout_seconds > timeout_seconds) {
		throw InvalidInputException("AI connect timeout must be between 1 and the total timeout");
	}
	return connect_timeout_seconds;
}

long RetryCount(const CompletionOptions &options) {
	if (options.has_retry_count) {
		return static_cast<long>(options.retry_count);
	}
	auto configured = GetEnv("DUCKDB_AI_RETRY_COUNT");
	if (configured.empty()) {
		return 0;
	}
	try {
		auto retry_count = std::stol(configured);
		if (retry_count < 0 || retry_count > 10) {
			throw InvalidInputException("DUCKDB_AI_RETRY_COUNT must be between 0 and 10");
		}
		return retry_count;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_RETRY_COUNT must be an integer between 0 and 10");
	}
}

long RetryBackoffMs(const CompletionOptions &options) {
	if (options.has_retry_backoff_ms) {
		return static_cast<long>(options.retry_backoff_ms);
	}
	auto configured = GetEnv("DUCKDB_AI_RETRY_BACKOFF_MS");
	if (configured.empty()) {
		return 1000;
	}
	try {
		auto retry_backoff_ms = std::stol(configured);
		if (retry_backoff_ms < 0 || retry_backoff_ms > 60000) {
			throw InvalidInputException("DUCKDB_AI_RETRY_BACKOFF_MS must be between 0 and 60000");
		}
		return retry_backoff_ms;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_RETRY_BACKOFF_MS must be an integer between 0 and 60000");
	}
}

int64_t MaxConcurrentRequests(const CompletionOptions &options) {
	if (options.has_max_concurrent_requests) {
		return options.max_concurrent_requests;
	}
	auto configured = GetEnv("DUCKDB_AI_MAX_CONCURRENT_REQUESTS");
	if (configured.empty()) {
		return 0;
	}
	try {
		auto max_concurrent_requests = std::stoll(configured);
		if (max_concurrent_requests < 0 || max_concurrent_requests > MAX_PROVIDER_CHUNK_WORKERS) {
			throw InvalidInputException("DUCKDB_AI_MAX_CONCURRENT_REQUESTS must be between 0 and %lld",
			                            static_cast<long long>(MAX_PROVIDER_CHUNK_WORKERS));
		}
		return max_concurrent_requests;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_MAX_CONCURRENT_REQUESTS must be an integer between 0 and %lld",
		                            static_cast<long long>(MAX_PROVIDER_CHUNK_WORKERS));
	}
}

int64_t MinRequestIntervalMs(const CompletionOptions &options) {
	if (options.has_min_request_interval_ms) {
		return options.min_request_interval_ms;
	}
	auto configured = GetEnv("DUCKDB_AI_MIN_REQUEST_INTERVAL_MS");
	if (configured.empty()) {
		return 0;
	}
	try {
		auto min_request_interval_ms = std::stoll(configured);
		if (min_request_interval_ms < 0 || min_request_interval_ms > 60000) {
			throw InvalidInputException("DUCKDB_AI_MIN_REQUEST_INTERVAL_MS must be between 0 and 60000");
		}
		return min_request_interval_ms;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_MIN_REQUEST_INTERVAL_MS must be an integer between 0 and 60000");
	}
}

int64_t TokenLimitPerMinute(const CompletionOptions &options) {
	if (options.has_token_limit_per_minute) {
		return options.token_limit_per_minute;
	}
	auto configured = GetEnv("DUCKDB_AI_TOKEN_LIMIT_PER_MINUTE");
	if (configured.empty()) {
		return 0;
	}
	try {
		auto token_limit_per_minute = std::stoll(configured);
		if (token_limit_per_minute < 0 || token_limit_per_minute > MAX_TOKEN_LIMIT_PER_MINUTE) {
			throw InvalidInputException("DUCKDB_AI_TOKEN_LIMIT_PER_MINUTE must be between 0 and %lld",
			                            static_cast<long long>(MAX_TOKEN_LIMIT_PER_MINUTE));
		}
		return token_limit_per_minute;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_TOKEN_LIMIT_PER_MINUTE must be an integer between 0 and %lld",
		                            static_cast<long long>(MAX_TOKEN_LIMIT_PER_MINUTE));
	}
}

void PruneProviderTokenWindow(ProviderRuntimeState &state, std::chrono::steady_clock::time_point now) {
	auto cutoff = now - std::chrono::minutes(1);
	while (!state.provider_token_window.empty() && state.provider_token_window.front().first <= cutoff) {
		state.provider_token_window_tokens -= state.provider_token_window.front().second;
		state.provider_token_window.pop_front();
	}
	if (state.provider_token_window_tokens < 0) {
		state.provider_token_window_tokens = 0;
	}
}

int64_t EstimatedCompletionTokens(const std::string &prompt, const CompletionOptions &options) {
	auto output_estimate = options.has_max_tokens ? options.max_tokens : DEFAULT_COMPLETION_OUTPUT_TOKEN_ESTIMATE;
	return EstimateTokenCount(prompt) + output_estimate;
}

int64_t TokenReservation(int64_t estimated_tokens, int64_t token_limit_per_minute) {
	if (estimated_tokens <= 0 || token_limit_per_minute <= 0) {
		return 0;
	}
	return std::min(estimated_tokens, token_limit_per_minute);
}

class ProviderRequestGuard {
public:
	explicit ProviderRequestGuard(const CompletionOptions &options, int64_t estimated_tokens)
	    : state(RuntimeState(options)), max_concurrent_requests(MaxConcurrentRequests(options)),
	      min_request_interval_ms(MinRequestIntervalMs(options)), token_limit_per_minute(TokenLimitPerMinute(options)),
	      request_tokens(TokenReservation(estimated_tokens, token_limit_per_minute)),
	      client_context(options.client_context) {
		if (max_concurrent_requests <= 0 && min_request_interval_ms <= 0 && token_limit_per_minute <= 0) {
			return;
		}

		std::unique_lock<std::mutex> lock(state.provider_control_mutex);
		while (true) {
			if (ClientInterrupted(client_context)) {
				throw InterruptException();
			}
			auto now = std::chrono::steady_clock::now();
			PruneProviderTokenWindow(state, now);
			auto concurrency_ready =
			    max_concurrent_requests <= 0 || state.active_provider_requests < max_concurrent_requests;
			auto rate_ready =
			    min_request_interval_ms <= 0 || !state.has_last_provider_request_start ||
			    now >= state.last_provider_request_start + std::chrono::milliseconds(min_request_interval_ms);
			auto token_ready = token_limit_per_minute <= 0 ||
			                   state.provider_token_window_tokens + request_tokens <= token_limit_per_minute;
			if (concurrency_ready && rate_ready && token_ready) {
				break;
			}
			if (!concurrency_ready) {
				state.provider_control_cv.wait_for(lock, std::chrono::milliseconds(100));
			} else {
				std::chrono::steady_clock::time_point wake_at;
				bool has_wake_at = false;
				auto set_wake_at = [&](std::chrono::steady_clock::time_point candidate) {
					if (!has_wake_at || candidate < wake_at) {
						wake_at = candidate;
						has_wake_at = true;
					}
				};
				if (!rate_ready) {
					set_wake_at(state.last_provider_request_start + std::chrono::milliseconds(min_request_interval_ms));
				}
				if (!token_ready && !state.provider_token_window.empty()) {
					set_wake_at(state.provider_token_window.front().first + std::chrono::minutes(1));
				}
				if (has_wake_at) {
					state.provider_control_cv.wait_until(lock, std::min(wake_at, now + std::chrono::milliseconds(100)));
				} else {
					state.provider_control_cv.wait_for(lock, std::chrono::milliseconds(100));
				}
			}
		}
		state.active_provider_requests++;
		auto request_start = std::chrono::steady_clock::now();
		if (min_request_interval_ms > 0) {
			state.last_provider_request_start = request_start;
			state.has_last_provider_request_start = true;
		}
		if (token_limit_per_minute > 0 && request_tokens > 0) {
			state.provider_token_window.emplace_back(request_start, request_tokens);
			state.provider_token_window_tokens += request_tokens;
		}
		acquired = true;
	}

	~ProviderRequestGuard() {
		if (!acquired) {
			return;
		}
		std::lock_guard<std::mutex> lock(state.provider_control_mutex);
		state.active_provider_requests--;
		state.provider_control_cv.notify_all();
	}

private:
	ProviderRuntimeState &state;
	int64_t max_concurrent_requests;
	int64_t min_request_interval_ms;
	int64_t token_limit_per_minute;
	int64_t request_tokens;
	ClientContext *client_context;
	bool acquired = false;
};

bool IsRetryableHttpStatus(long status) {
	return status == 429 || status >= 500;
}

long RetryDelayMs(long base_backoff_ms, long attempt, long retry_after_ms) {
	if (retry_after_ms >= 0) {
		return retry_after_ms;
	}
	if (base_backoff_ms <= 0) {
		return 0;
	}
	auto capped_attempt = std::min<long>(attempt, 10);
	auto exponential = base_backoff_ms;
	for (long i = 0; i < capped_attempt; i++) {
		if (exponential >= 60000) {
			exponential = 60000;
			break;
		}
		exponential *= 2;
	}
	exponential = std::min<long>(exponential, 60000);
	static thread_local std::mt19937 generator(std::random_device {}());
	std::uniform_int_distribution<long> jitter(0, std::max<long>(1, exponential / 4));
	return std::min<long>(60000, exponential + jitter(generator));
}

void SleepBeforeRetry(long retry_delay_ms, ClientContext *client_context) {
	if (retry_delay_ms <= 0) {
		return;
	}
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(retry_delay_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		if (ClientInterrupted(client_context)) {
			throw InterruptException();
		}
		auto remaining =
		    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(100)));
	}
}

HttpResponse HttpPost(const std::string &url, const std::string &payload, const std::vector<std::string> &headers,
                      long timeout_seconds, bool throw_http_errors = true, long retry_count = 0,
                      long retry_backoff_ms = 1000, ClientContext *client_context = nullptr,
                      long connect_timeout_seconds = 0) {
	EnsureCurlGlobalInit();
	int64_t total_elapsed_ms = 0;
	for (long attempt = 0; attempt <= retry_count; attempt++) {
		auto curl = ThreadLocalCurlHandle();
		std::string response_body;
		CurlHeaderCapture header_capture;
		CurlProgressState progress_state {client_context};
		struct curl_slist *header_list = nullptr;
		for (auto &header : headers) {
			header_list = curl_slist_append(header_list, header.c_str());
		}

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
		curl_easy_setopt(curl, CURLOPT_SHARE, SharedCurlHandle());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_capture);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
		                 connect_timeout_seconds > 0 ? connect_timeout_seconds
		                                             : ConnectTimeoutSeconds(timeout_seconds));
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_state);

		auto start = std::chrono::steady_clock::now();
		auto result = curl_easy_perform(curl);
		auto end = std::chrono::steady_clock::now();
		long status = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

		curl_slist_free_all(header_list);

		total_elapsed_ms += std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
		if (attempt < retry_count && (result != CURLE_OK || IsRetryableHttpStatus(status))) {
			if (result == CURLE_ABORTED_BY_CALLBACK && ClientInterrupted(client_context)) {
				throw InterruptException();
			}
			SleepBeforeRetry(RetryDelayMs(retry_backoff_ms, attempt, header_capture.retry_after_ms), client_context);
			continue;
		}
		if (result == CURLE_ABORTED_BY_CALLBACK && ClientInterrupted(client_context)) {
			throw InterruptException();
		}
		if (result != CURLE_OK) {
			throw IOException("AI provider request failed: %s", curl_easy_strerror(result));
		}
		if (throw_http_errors && (status < 200 || status >= 300)) {
			throw IOException("AI provider returned HTTP %ld: %s", status, response_body);
		}
		auto response = HttpResponse(response_body, status, total_elapsed_ms, header_capture.retry_after_ms);
		response.retries = attempt;
		return response;
	}
	throw InternalException("AI provider retry loop exited unexpectedly");
}

void UsageLogWorkerLoop(ProviderRuntimeState *state) {
	while (true) {
		UsageLogJob job;
		{
			std::unique_lock<std::mutex> lock(state->usage_log_mutex);
			state->usage_log_cv.wait(lock,
			                         [&]() { return state->usage_log_shutdown || !state->usage_log_queue.empty(); });
			if (state->usage_log_shutdown) {
				// Non-strict usage logs are best-effort. Draining the queue here would block database
				// shutdown on up to MAX_USAGE_LOG_QUEUE network posts, so drop what is left instead.
				state->usage_log_queue.clear();
				return;
			}
			job = std::move(state->usage_log_queue.front());
			state->usage_log_queue.pop_front();
		}
		try {
			HttpPost(job.url, job.payload, job.headers, job.timeout_seconds, true, job.retry_count,
			         job.retry_backoff_ms);
		} catch (...) {
			// Non-strict usage logs are best-effort and must not fail later queries from the worker thread.
		}
	}
}

void StartUsageLogWorkerLocked(ProviderRuntimeState &state) {
	if (state.usage_log_worker_started) {
		return;
	}
	state.usage_log_worker = std::thread(UsageLogWorkerLoop, &state);
	state.usage_log_worker_started = true;
}

void EnqueueUsageLog(ProviderRuntimeState &state, UsageLogJob job) {
	{
		std::lock_guard<std::mutex> lock(state.usage_log_mutex);
		StartUsageLogWorkerLocked(state);
		if (state.usage_log_queue.size() >= MAX_USAGE_LOG_QUEUE) {
			state.usage_log_queue.pop_front();
		}
		state.usage_log_queue.push_back(std::move(job));
	}
	state.usage_log_cv.notify_one();
}

void StopUsageLogWorker(ProviderRuntimeState &state) {
	{
		std::lock_guard<std::mutex> lock(state.usage_log_mutex);
		state.usage_log_shutdown = true;
	}
	state.usage_log_cv.notify_all();
	if (state.usage_log_worker_started && state.usage_log_worker.joinable()) {
		state.usage_log_worker.join();
	}
}

ProviderRuntimeState::~ProviderRuntimeState() {
	StopUsageLogWorker(*this);
}

HttpResponse ProviderHttpPost(const std::string &url, const std::string &payload,
                              const std::vector<std::string> &headers, const CompletionOptions &options,
                              int64_t estimated_tokens) {
	auto retry_count = RetryCount(options);
	auto retry_backoff_ms = RetryBackoffMs(options);
	auto timeout_seconds = TimeoutSeconds(options);
	auto connect_timeout_seconds = ConnectTimeoutSeconds(options, timeout_seconds);
	int64_t total_elapsed_ms = 0;
	for (long attempt = 0; attempt <= retry_count; attempt++) {
		try {
			HttpResponse response;
			{
				ProviderRequestGuard request_guard(options, estimated_tokens);
				response = HttpPost(url, payload, headers, timeout_seconds, false, 0, retry_backoff_ms,
				                    options.client_context, connect_timeout_seconds);
			}
			total_elapsed_ms += response.elapsed_ms;
			response.elapsed_ms = total_elapsed_ms;
			response.retries = attempt;
			if (attempt < retry_count && IsRetryableHttpStatus(response.status)) {
				SleepBeforeRetry(RetryDelayMs(retry_backoff_ms, attempt, response.retry_after_ms),
				                 options.client_context);
				continue;
			}
			return response;
		} catch (InterruptException &) {
			throw;
		} catch (std::exception &) {
			if (attempt >= retry_count) {
				throw;
			}
			SleepBeforeRetry(RetryDelayMs(retry_backoff_ms, attempt, -1), options.client_context);
		}
	}
	throw InternalException("AI provider retry loop exited unexpectedly");
}

std::string RedactValue(std::string input, const std::string &secret) {
	if (secret.empty()) {
		return input;
	}
	size_t pos = 0;
	while ((pos = input.find(secret, pos)) != std::string::npos) {
		input.replace(pos, secret.size(), "[redacted]");
		pos += 10;
	}
	return input;
}

std::string RedactProviderSecrets(const ProviderConfig &config, const std::string &input) {
	return RedactValue(input, config.api_key);
}

std::string ProviderErrorDetail(const ProviderConfig &config, const HttpResponse &response) {
	auto body = RedactProviderSecrets(config, response.body);
	std::string message;
	std::string error_type;
	std::string error_code;
	FindJsonStringValue(body, "message", message);
	if (message.empty()) {
		FindJsonStringValue(body, "error", message);
	}
	if (message.empty()) {
		FindJsonStringValue(body, "detail", message);
	}
	FindJsonStringValue(body, "type", error_type);
	FindJsonStringValue(body, "code", error_code);

	std::string detail = "AI provider \"" + config.provider + "\" (" + config.protocol + ", model \"" + config.model +
	                     "\") returned HTTP " + std::to_string(response.status);
	if (!error_type.empty() || !error_code.empty()) {
		detail += " [";
		bool wrote = false;
		if (!error_type.empty()) {
			detail += "type=" + error_type;
			wrote = true;
		}
		if (!error_code.empty()) {
			if (wrote) {
				detail += ", ";
			}
			detail += "code=" + error_code;
		}
		detail += "]";
	}
	if (!message.empty()) {
		detail += ": " + message;
	} else if (!body.empty()) {
		detail += ": " + body;
	}
	return detail;
}

void ThrowIfProviderHttpError(const ProviderConfig &config, const HttpResponse &response) {
	if (response.status >= 200 && response.status < 300) {
		return;
	}
	throw IOException("%s", ProviderErrorDetail(config, response));
}

std::string ProviderSpecificEnvPrefix(const std::string &provider) {
	auto upper = provider;
	std::transform(upper.begin(), upper.end(), upper.begin(),
	               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	return upper;
}

ProviderConfig ProviderDefaults(const std::string &provider_input) {
	auto provider =
	    NormalizeProviderNameInternal(provider_input.empty() ? GetEnv("DUCKDB_AI_PROVIDER") : provider_input);
	if (provider.empty()) {
		provider = "ollama";
	}

	if (provider == "openai") {
		return {provider, "openai_chat", "gpt-4o-mini", "", "https://api.openai.com/v1", "OPENAI_API_KEY", "", true};
	}
	if (provider == "openrouter") {
		return {provider, "openai_chat", "openai/gpt-4o-mini", "", "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY",
		        "",       true};
	}
	if (provider == "databricks") {
		return {provider, "openai_chat", "databricks-llama-4-maverick", "", "", "DATABRICKS_TOKEN", "", true};
	}
	if (provider == "snowflake") {
		return {provider, "openai_chat", "claude-sonnet-4-5", "", "", "SNOWFLAKE_PAT", "", true};
	}
	if (provider == "openai_privacy_filter") {
		return {provider,
		        "privacy_filter",
		        "openai/privacy-filter",
		        "",
		        "http://localhost:8080",
		        "OPENAI_PRIVACY_FILTER_API_KEY",
		        "",
		        false};
	}
	if (provider == "openai_compatible") {
		return {provider, "openai_chat", "gpt-4o-mini", "", "", "OPENAI_COMPATIBLE_API_KEY", "", false};
	}
	if (provider == "llamacpp") {
		// llama.cpp llama-server speaks the OpenAI-compatible protocol and ignores the request model
		// name unless started with --model-alias; the loaded model always answers.
		return {provider, "openai_chat", "default", "", "http://localhost:8080/v1", "LLAMACPP_API_KEY", "", false};
	}
	if (provider == "azure") {
		return {provider, "openai_chat", "gpt-4o", "", "", "AZURE_OPENAI_API_KEY", "", true};
	}
	if (provider == "deepseek") {
		return {provider, "openai_chat", "deepseek-v4-flash", "", "https://api.deepseek.com", "DEEPSEEK_API_KEY",
		        "",       true};
	}
	if (provider == "mistral") {
		return {provider, "openai_chat", "mistral-small-latest", "", "https://api.mistral.ai/v1", "MISTRAL_API_KEY",
		        "",       true};
	}
	if (provider == "zai" || provider == "zhipu") {
		return {"zai", "openai_chat", "glm-4.7-flash", "", "https://api.z.ai/api/paas/v4", "ZAI_API_KEY", "", true};
	}
	if (provider == "gemini") {
		return {provider,
		        "openai_chat",
		        "gemini-3.5-flash",
		        "",
		        "https://generativelanguage.googleapis.com/v1beta/openai",
		        "GEMINI_API_KEY",
		        "",
		        true};
	}
	if (provider == "anthropic" || provider == "claude") {
		return {"anthropic",
		        "anthropic_messages",
		        "claude-haiku-4-5",
		        "",
		        "https://api.anthropic.com/v1",
		        "ANTHROPIC_API_KEY",
		        "",
		        true};
	}
	if (provider == "ollama") {
		return {provider, "ollama_chat", "llama3.2", "", "http://localhost:11434", "OLLAMA_API_KEY", "", false};
	}

	throw InvalidInputException(
	    "Unsupported AI provider \"%s\". Supported providers: ollama, openai, azure, "
	    "anthropic, claude, databricks, gemini, gcp, mistral, snowflake, zai, deepseek, openrouter, "
	    "openai_privacy_filter, openai_compatible, local, llamacpp, llama.cpp",
	    provider_input);
}

std::string ResolveBaseUrl(const ProviderConfig &config) {
	auto provider_prefix = ProviderSpecificEnvPrefix(config.provider);
	auto provider_base_url = GetEnv(provider_prefix + "_BASE_URL");
	auto generic_base_url = GetEnv("DUCKDB_AI_BASE_URL");
	if (config.provider == "azure") {
		auto azure_base_url = GetEnv("AZURE_OPENAI_BASE_URL");
		auto azure_endpoint = GetEnv("AZURE_OPENAI_ENDPOINT");
		if (!azure_base_url.empty()) {
			return AzureOpenAIBaseUrl(azure_base_url);
		}
		if (!azure_endpoint.empty()) {
			return AzureOpenAIBaseUrl(azure_endpoint);
		}
	}
	if (!provider_base_url.empty()) {
		return config.provider == "azure"        ? AzureOpenAIBaseUrl(provider_base_url)
		       : config.provider == "databricks" ? DatabricksBaseUrl(provider_base_url)
		       : config.provider == "snowflake"  ? SnowflakeCortexBaseUrl(provider_base_url)
		                                         : TrimTrailingSlash(provider_base_url);
	}
	if (!generic_base_url.empty()) {
		return config.provider == "azure"        ? AzureOpenAIBaseUrl(generic_base_url)
		       : config.provider == "databricks" ? DatabricksBaseUrl(generic_base_url)
		       : config.provider == "snowflake"  ? SnowflakeCortexBaseUrl(generic_base_url)
		                                         : TrimTrailingSlash(generic_base_url);
	}
	if (config.provider == "anthropic") {
		auto claude_base_url = GetEnv("CLAUDE_BASE_URL");
		if (!claude_base_url.empty()) {
			return TrimTrailingSlash(claude_base_url);
		}
	}
	if (config.provider == "databricks") {
		auto databricks_host = GetEnv("DATABRICKS_HOST");
		if (!databricks_host.empty()) {
			return DatabricksBaseUrl(databricks_host);
		}
	}
	if (config.provider == "snowflake") {
		auto snowflake_account_url = GetEnv("SNOWFLAKE_ACCOUNT_URL");
		if (!snowflake_account_url.empty()) {
			return SnowflakeCortexBaseUrl(snowflake_account_url);
		}
		auto snowflake_host = GetEnv("SNOWFLAKE_HOST");
		if (!snowflake_host.empty()) {
			return SnowflakeCortexBaseUrl(snowflake_host);
		}
		auto snowflake_account = GetEnv("SNOWFLAKE_ACCOUNT");
		if (!snowflake_account.empty()) {
			return SnowflakeAccountBaseUrl(snowflake_account);
		}
	}
	if (config.provider == "ollama") {
		auto ollama_host = GetEnv("OLLAMA_HOST");
		if (!ollama_host.empty()) {
			return TrimTrailingSlash(ollama_host);
		}
	}
	return TrimTrailingSlash(config.base_url);
}

std::string ResolveApiKey(const ProviderConfig &config) {
	auto provider_prefix = ProviderSpecificEnvPrefix(config.provider);
	auto provider_key = GetEnv(provider_prefix + "_API_KEY");
	auto generic_key = GetEnv("DUCKDB_AI_API_KEY");
	if (config.provider == "azure") {
		auto azure_key = GetEnv("AZURE_OPENAI_API_KEY");
		if (!azure_key.empty()) {
			return azure_key;
		}
	}
	if (!provider_key.empty()) {
		return provider_key;
	}
	if (!generic_key.empty()) {
		return generic_key;
	}
	if (config.provider == "anthropic") {
		auto claude_key = GetEnv("CLAUDE_API_KEY");
		if (!claude_key.empty()) {
			return claude_key;
		}
	}
	if (config.provider == "databricks") {
		auto databricks_token = GetEnv("DATABRICKS_TOKEN");
		if (!databricks_token.empty()) {
			return databricks_token;
		}
	}
	if (config.provider == "snowflake") {
		auto snowflake_pat = GetEnv("SNOWFLAKE_PAT");
		if (!snowflake_pat.empty()) {
			return snowflake_pat;
		}
		auto snowflake_token = GetEnv("SNOWFLAKE_TOKEN");
		if (!snowflake_token.empty()) {
			return snowflake_token;
		}
	}
	return GetEnv(config.api_key_env);
}

std::string ResolveModel(const ProviderConfig &config, const std::string &model_input) {
	if (!model_input.empty()) {
		return model_input;
	}
	auto provider_prefix = ProviderSpecificEnvPrefix(config.provider);
	auto provider_model = GetEnv(provider_prefix + "_MODEL");
	auto generic_model = GetEnv("DUCKDB_AI_MODEL");
	if (config.provider == "azure") {
		auto azure_model = GetEnv("AZURE_OPENAI_MODEL");
		auto azure_deployment = GetEnv("AZURE_OPENAI_DEPLOYMENT");
		if (!azure_model.empty()) {
			return azure_model;
		}
		if (!azure_deployment.empty()) {
			return azure_deployment;
		}
	}
	if (!provider_model.empty()) {
		return provider_model;
	}
	if (!generic_model.empty()) {
		return generic_model;
	}
	if (config.provider == "anthropic") {
		auto claude_model = GetEnv("CLAUDE_MODEL");
		if (!claude_model.empty()) {
			return claude_model;
		}
	}
	return config.default_model;
}

ProviderConfig ResolveProviderConfig(const CompletionOptions &options, bool require_api_key) {
	auto config = ProviderDefaults(options.provider);
	config.base_url = options.base_url.empty()          ? ResolveBaseUrl(config)
	                  : config.provider == "azure"      ? AzureOpenAIBaseUrl(options.base_url)
	                  : config.provider == "databricks" ? DatabricksBaseUrl(options.base_url)
	                  : config.provider == "snowflake"  ? SnowflakeCortexBaseUrl(options.base_url)
	                                                    : TrimTrailingSlash(options.base_url);
	config.api_key = options.api_key.empty() ? ResolveApiKey(config) : options.api_key;
	config.model = ResolveModel(config, options.model);
	if (require_api_key && config.requires_api_key && config.api_key.empty()) {
		throw InvalidInputException("AI provider \"%s\" requires an API key. Set %s or DUCKDB_AI_API_KEY.",
		                            config.provider, config.api_key_env);
	}
	if (require_api_key && config.provider == "databricks" && config.base_url.empty()) {
		throw InvalidInputException(
		    "AI provider \"databricks\" requires a base URL. Set duckdb_ai_base_url, DATABRICKS_BASE_URL, or "
		    "DATABRICKS_HOST.");
	}
	if (require_api_key && config.provider == "snowflake" && config.base_url.empty()) {
		throw InvalidInputException("AI provider \"snowflake\" requires a base URL. Set duckdb_ai_base_url, "
		                            "SNOWFLAKE_BASE_URL, SNOWFLAKE_ACCOUNT_URL, SNOWFLAKE_HOST, or "
		                            "SNOWFLAKE_ACCOUNT.");
	}
	return config;
}

std::string RequestEndpoint(const ProviderConfig &config) {
	if (config.protocol == "privacy_filter") {
		if (EndsWith(config.base_url, "/redact")) {
			return config.base_url;
		}
		return config.base_url + "/redact";
	}
	if (config.protocol == "ollama_chat") {
		return config.base_url + "/api/chat";
	}
	if (config.protocol == "anthropic_messages") {
		return config.base_url + "/messages";
	}
	if (EndsWith(config.base_url, "/chat/completions")) {
		return config.base_url;
	}
	return config.base_url + "/chat/completions";
}

std::string ChatMessagesJson(const std::string &prompt, const std::string &system_prompt) {
	auto escaped_prompt = JsonEscape(prompt);
	std::string messages = "[";
	if (!system_prompt.empty()) {
		messages += "{\"role\":\"system\",\"content\":\"" + JsonEscape(system_prompt) + "\"},";
	}
	messages += "{\"role\":\"user\",\"content\":\"" + escaped_prompt + "\"}]";
	return messages;
}

bool ValidateJsonDocumentInternal(const std::string &input, std::string &error) {
	JsonValue value;
	return ParseJsonValueDocument(input, value, error);
}

size_t FirstNonWhitespace(const std::string &input) {
	size_t pos = 0;
	while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
		pos++;
	}
	return pos;
}

std::string NormalizeResponseFormat(const CompletionOptions &options) {
	auto format = LowerAscii(options.response_format);
	if (format.empty()) {
		return "";
	}
	if (format == "text" || format == "json_object" || format == "json_schema") {
		return format;
	}
	throw InvalidInputException("AI option \"response_format\" must be one of: text, json_object, json_schema");
}

void ValidateResponseSchema(const CompletionOptions &options) {
	if (options.response_schema.empty()) {
		return;
	}
	std::string error;
	if (!ValidateJsonDocumentInternal(options.response_schema, error)) {
		throw InvalidInputException("AI option \"response_schema\" must be valid JSON: %s", error);
	}
	auto first = FirstNonWhitespace(options.response_schema);
	if (first == options.response_schema.size() || options.response_schema[first] != '{') {
		throw InvalidInputException("AI option \"response_schema\" must be a JSON object");
	}
}

std::string OpenAIResponseFormatJson(const CompletionOptions &options) {
	ValidateResponseSchema(options);
	if (!options.response_schema.empty()) {
		return "\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"name\":\"duckdb_ai_response\","
		       "\"schema\":" +
		       options.response_schema + ",\"strict\":true}}";
	}
	auto format = NormalizeResponseFormat(options);
	if (format.empty() || format == "text") {
		return "";
	}
	if (format == "json_schema") {
		throw InvalidInputException(
		    "AI option \"response_schema\" must be provided when response_format is json_schema");
	}
	return "\"response_format\":{\"type\":\"json_object\"}";
}

std::string OllamaFormatJson(const CompletionOptions &options) {
	ValidateResponseSchema(options);
	if (!options.response_schema.empty()) {
		return "\"format\":" + options.response_schema;
	}
	auto format = NormalizeResponseFormat(options);
	if (format.empty() || format == "text") {
		return "";
	}
	if (format == "json_schema") {
		throw InvalidInputException(
		    "AI option \"response_schema\" must be provided when response_format is json_schema");
	}
	return "\"format\":\"json\"";
}

std::string RequestPayload(const ProviderConfig &config, const std::string &prompt, const CompletionOptions &options) {
	auto escaped_model = JsonEscape(config.model);
	if (config.protocol == "privacy_filter") {
		return "{\"text\":\"" + JsonEscape(prompt) + "\",\"model\":\"" + escaped_model + "\"}";
	}
	if (config.protocol == "anthropic_messages") {
		ValidateResponseSchema(options);
		auto format = NormalizeResponseFormat(options);
		if (!options.response_schema.empty()) {
			throw InvalidInputException(
			    "AI option \"response_schema\" is not supported for provider \"%s\". Use an OpenAI-compatible or "
			    "Ollama provider for enforced JSON Schema output.",
			    config.provider);
		}
		if (format == "json_schema" && options.response_schema.empty()) {
			throw InvalidInputException(
			    "AI option \"response_schema\" must be provided when response_format is json_schema");
		}
		auto max_tokens = options.has_max_tokens ? options.max_tokens : 4096;
		auto payload = "{\"model\":\"" + escaped_model + "\",\"max_tokens\":" + std::to_string(max_tokens);
		if (!options.system_prompt.empty()) {
			if (PromptCacheEnabled(options)) {
				payload += ",\"system\":[{\"type\":\"text\",\"text\":\"" + JsonEscape(options.system_prompt) +
				           "\",\"cache_control\":{\"type\":\"ephemeral\"}}]";
			} else {
				payload += ",\"system\":\"" + JsonEscape(options.system_prompt) + "\"";
			}
		}
		if (options.has_temperature) {
			payload += ",\"temperature\":" + JsonDouble(options.temperature);
		}
		payload += ",\"messages\":[{\"role\":\"user\",\"content\":\"" + JsonEscape(prompt) + "\"}]}";
		return payload;
	}
	if (config.protocol == "ollama_chat") {
		auto payload = "{\"model\":\"" + escaped_model +
		               "\",\"messages\":" + ChatMessagesJson(prompt, options.system_prompt) + ",\"stream\":false";
		auto format = OllamaFormatJson(options);
		if (!format.empty()) {
			payload += "," + format;
		}
		if (options.has_temperature || options.has_max_tokens) {
			payload += ",\"options\":{";
			bool wrote_option = false;
			if (options.has_temperature) {
				payload += "\"temperature\":" + JsonDouble(options.temperature);
				wrote_option = true;
			}
			if (options.has_max_tokens) {
				if (wrote_option) {
					payload += ",";
				}
				payload += "\"num_predict\":" + std::to_string(options.max_tokens);
			}
			payload += "}";
		}
		payload += "}";
		return payload;
	}
	auto temperature = options.has_temperature ? options.temperature : 0.1;
	auto payload = "{\"model\":\"" + escaped_model +
	               "\",\"messages\":" + ChatMessagesJson(prompt, options.system_prompt) +
	               ",\"temperature\":" + JsonDouble(temperature);
	if (options.has_max_tokens) {
		payload += ",\"max_tokens\":" + std::to_string(options.max_tokens);
	}
	auto response_format = OpenAIResponseFormatJson(options);
	if (!response_format.empty()) {
		payload += "," + response_format;
	}
	if (config.provider == "openai" && PromptCacheEnabled(options)) {
		auto prompt_cache_key = PromptCacheKey(config, options);
		if (!prompt_cache_key.empty()) {
			payload += ",\"prompt_cache_key\":\"" + JsonEscape(prompt_cache_key) + "\"";
		}
	}
	payload += "}";
	return payload;
}

std::string EmbeddingDefaultModel(const std::string &provider) {
	if (provider == "ollama") {
		return "nomic-embed-text";
	}
	if (provider == "openai") {
		return "text-embedding-3-small";
	}
	if (provider == "openrouter") {
		return "openai/text-embedding-3-small";
	}
	if (provider == "azure") {
		return "text-embedding-3-small";
	}
	if (provider == "openai_compatible") {
		return "text-embedding-3-small";
	}
	if (provider == "llamacpp") {
		// llama-server exposes /v1/embeddings when started with --embeddings and uses the loaded model.
		return "default";
	}
	if (provider == "mistral") {
		return "mistral-embed";
	}
	if (provider == "gemini") {
		return "gemini-embedding-001";
	}
	if (provider == "zai") {
		return "embedding-3";
	}
	throw InvalidInputException("AI provider \"%s\" does not have a configured embedding endpoint. Use provider "
	                            "\"openai\" with base_url for OpenAI-compatible embedding gateways.",
	                            provider);
}

ProviderConfig ResolveEmbeddingProviderConfig(const CompletionOptions &options, bool require_api_key) {
	auto config = ProviderDefaults(options.provider);
	config.default_model = EmbeddingDefaultModel(config.provider);
	config.protocol = config.provider == "ollama" ? "ollama_embed" : "openai_embeddings";
	config.base_url = options.base_url.empty()     ? ResolveBaseUrl(config)
	                  : config.provider == "azure" ? AzureOpenAIBaseUrl(options.base_url)
	                                               : TrimTrailingSlash(options.base_url);
	config.api_key = options.api_key.empty() ? ResolveApiKey(config) : options.api_key;
	config.model = ResolveModel(config, options.model);
	if (require_api_key && config.requires_api_key && config.api_key.empty()) {
		throw InvalidInputException("AI provider \"%s\" requires an API key. Set %s or DUCKDB_AI_API_KEY.",
		                            config.provider, config.api_key_env);
	}
	return config;
}

std::string EmbeddingEndpoint(const ProviderConfig &config) {
	if (config.protocol == "ollama_embed") {
		return config.base_url + "/api/embed";
	}
	return config.base_url + "/embeddings";
}

std::string EmbeddingPayload(const ProviderConfig &config, const std::string &input) {
	return "{\"model\":\"" + JsonEscape(config.model) + "\",\"input\":\"" + JsonEscape(input) + "\"}";
}

std::string EmbeddingPayload(const ProviderConfig &config, const std::vector<std::string> &inputs) {
	if (inputs.size() == 1) {
		return EmbeddingPayload(config, inputs[0]);
	}
	std::string payload = "{\"model\":\"" + JsonEscape(config.model) + "\",\"input\":[";
	for (idx_t i = 0; i < inputs.size(); i++) {
		if (i > 0) {
			payload += ",";
		}
		payload += "\"" + JsonEscape(inputs[i]) + "\"";
	}
	payload += "]}";
	return payload;
}

std::vector<double> FindEmbeddingArray(const std::string &body, const std::string &key) {
	std::string error;
	auto doc = ReadYyjsonDocument(body, error);
	if (!doc) {
		return {};
	}
	std::vector<double> values;
	if (FindYyjsonNumberArrayField(duckdb_yyjson::yyjson_doc_get_root(doc.get()), key, values)) {
		return values;
	}
	return {};
}

std::vector<std::vector<double>> ParseEmbeddingArrays(const ProviderConfig &config, const std::string &body) {
	std::string error;
	auto doc = ReadYyjsonDocument(body, error);
	if (!doc) {
		return {};
	}
	auto root = duckdb_yyjson::yyjson_doc_get_root(doc.get());
	std::vector<std::vector<double>> embeddings;
	if (config.protocol == "ollama_embed") {
		auto embedding_values = YyjsonObjectGet(root, "embeddings");
		if (duckdb_yyjson::yyjson_is_arr(embedding_values)) {
			duckdb_yyjson::yyjson_val *entry;
			size_t index;
			size_t max;
			yyjson_arr_foreach(embedding_values, index, max, entry) {
				std::vector<double> values;
				if (YyjsonNumericArray(entry, values)) {
					embeddings.push_back(std::move(values));
				}
			}
		}
		return embeddings;
	}
	auto data = YyjsonObjectGet(root, "data");
	if (duckdb_yyjson::yyjson_is_arr(data)) {
		duckdb_yyjson::yyjson_val *entry;
		size_t index;
		size_t max;
		yyjson_arr_foreach(data, index, max, entry) {
			std::vector<double> values;
			if (YyjsonDirectNumberArray(entry, "embedding", values)) {
				embeddings.push_back(std::move(values));
			}
		}
	}
	return embeddings;
}

EmbeddingResult ParseEmbeddingResult(const ProviderConfig &config, const HttpResponse &response) {
	EmbeddingResult result;
	result.values = FindEmbeddingArray(response.body, config.protocol == "ollama_embed" ? "embeddings" : "embedding");
	if (result.values.empty() && config.protocol != "ollama_embed") {
		result.values = FindEmbeddingArray(response.body, "embeddings");
	}
	if (result.values.empty()) {
		throw IOException("AI provider embedding response contained an empty embedding: %s", response.body);
	}
	result.raw_response = response.body;
	result.http_status = response.status;
	result.elapsed_ms = response.elapsed_ms;
	result.retries = response.retries;
	result.cache_hit = response.cache_hit;
	result.prompt_tokens = FindJsonIntegerValue(response.body, "prompt_tokens");
	result.total_tokens = FindJsonIntegerValue(response.body, "total_tokens");
	return result;
}

std::vector<EmbeddingResult> ParseEmbeddingResults(const ProviderConfig &config, const HttpResponse &response,
                                                   idx_t expected_count) {
	auto values = ParseEmbeddingArrays(config, response.body);
	if (values.size() != expected_count) {
		if (expected_count == 1) {
			return {ParseEmbeddingResult(config, response)};
		}
		throw IOException("AI provider embedding response returned %llu embeddings for %llu inputs",
		                  static_cast<unsigned long long>(values.size()),
		                  static_cast<unsigned long long>(expected_count));
	}
	auto prompt_tokens = FindJsonIntegerValue(response.body, "prompt_tokens");
	auto total_tokens = FindJsonIntegerValue(response.body, "total_tokens");
	std::vector<EmbeddingResult> results;
	results.reserve(values.size());
	for (idx_t i = 0; i < values.size(); i++) {
		EmbeddingResult result;
		result.values = std::move(values[i]);
		result.raw_response = response.body;
		result.http_status = response.status;
		result.elapsed_ms = response.elapsed_ms;
		result.retries = response.retries;
		result.cache_hit = response.cache_hit;
		if (prompt_tokens >= 0) {
			result.prompt_tokens = static_cast<int64_t>(
			    std::ceil(static_cast<double>(prompt_tokens) / static_cast<double>(std::max<idx_t>(1, values.size()))));
		} else {
			result.prompt_tokens = -1;
		}
		if (total_tokens >= 0) {
			result.total_tokens = static_cast<int64_t>(
			    std::ceil(static_cast<double>(total_tokens) / static_cast<double>(std::max<idx_t>(1, values.size()))));
		} else {
			result.total_tokens = -1;
		}
		results.push_back(std::move(result));
	}
	return results;
}

std::vector<std::string> RequestHeaders(const ProviderConfig &config) {
	std::vector<std::string> headers {"Content-Type: application/json", "User-Agent: " + ExtensionUserAgent()};
	if (config.protocol == "anthropic_messages") {
		headers.push_back("anthropic-version: 2023-06-01");
		headers.push_back("x-api-key: " + config.api_key);
		return headers;
	}
	if (config.provider == "azure") {
		if (!config.api_key.empty()) {
			headers.push_back("api-key: " + config.api_key);
		}
		return headers;
	}
	if (!config.api_key.empty()) {
		headers.push_back("Authorization: Bearer " + config.api_key);
	}
	if (config.provider == "openrouter") {
		auto referer = GetEnv("OPENROUTER_HTTP_REFERER");
		auto title = GetEnv("OPENROUTER_X_TITLE");
		if (!referer.empty()) {
			headers.push_back("HTTP-Referer: " + referer);
		}
		if (!title.empty()) {
			headers.push_back("X-OpenRouter-Title: " + title);
		}
	}
	return headers;
}

std::string ExtractCompletionText(const ProviderConfig &config, duckdb_yyjson::yyjson_val *root,
                                  const std::string &body) {
	std::string value;
	if (root) {
		if (config.protocol == "privacy_filter") {
			if (YyjsonDirectString(root, "redacted_text", value) || YyjsonDirectString(root, "masked_text", value) ||
			    YyjsonDirectString(root, "text", value) || YyjsonDirectString(root, "output", value)) {
				return value;
			}
		} else if (config.protocol == "anthropic_messages") {
			auto content = YyjsonObjectGet(root, "content");
			if (duckdb_yyjson::yyjson_is_arr(content)) {
				duckdb_yyjson::yyjson_val *entry;
				size_t index;
				size_t max;
				yyjson_arr_foreach(content, index, max, entry) {
					std::string type;
					if (YyjsonDirectString(entry, "type", type) && type != "text") {
						continue;
					}
					if (YyjsonDirectString(entry, "text", value)) {
						return value;
					}
				}
			}
		} else if (config.protocol == "ollama_chat") {
			auto message = YyjsonObjectGet(root, "message");
			if (YyjsonDirectString(message, "content", value)) {
				return value;
			}
		} else {
			auto first_choice = YyjsonArrayGet(YyjsonObjectGet(root, "choices"), 0);
			auto message = YyjsonObjectGet(first_choice, "message");
			if (YyjsonDirectString(message, "content", value)) {
				return value;
			}
			if (YyjsonDirectString(message, "refusal", value)) {
				throw IOException("AI provider response contained a refusal instead of completion text: %s", value);
			}
		}
		// Fallback for gateways with non-standard response shapes: recursive search over the
		// already-parsed document.
		if (config.protocol == "privacy_filter") {
			if (FindYyjsonStringField(root, "redacted_text", value) ||
			    FindYyjsonStringField(root, "masked_text", value) || FindYyjsonStringField(root, "text", value) ||
			    FindYyjsonStringField(root, "output", value)) {
				return value;
			}
		} else if (config.protocol == "anthropic_messages") {
			if (FindYyjsonStringField(root, "text", value)) {
				return value;
			}
		} else if (FindYyjsonStringField(root, "content", value)) {
			return value;
		}
	}
	throw IOException("AI provider response did not contain a supported completion text field: %s", body);
}

CompletionResult ParseCompletionResult(const ProviderConfig &config, const HttpResponse &response) {
	std::string error;
	auto doc = ReadYyjsonDocument(response.body, error);
	auto root = doc ? duckdb_yyjson::yyjson_doc_get_root(doc.get()) : nullptr;

	CompletionResult result;
	result.text = ExtractCompletionText(config, root, response.body);
	result.raw_response = response.body;
	result.http_status = response.status;
	result.elapsed_ms = response.elapsed_ms;
	result.retries = response.retries;
	result.cache_hit = response.cache_hit;
	result.prompt_tokens = -1;
	result.completion_tokens = -1;
	result.total_tokens = -1;
	result.cached_prompt_tokens = -1;
	result.cache_creation_prompt_tokens = -1;

	if (config.protocol == "anthropic_messages") {
		auto usage = YyjsonObjectGet(root, "usage");
		result.prompt_tokens = YyjsonIntegerOrMissing(usage, "input_tokens");
		result.completion_tokens = YyjsonIntegerOrMissing(usage, "output_tokens");
		result.cached_prompt_tokens = YyjsonIntegerOrMissing(usage, "cache_read_input_tokens");
		result.cache_creation_prompt_tokens = YyjsonIntegerOrMissing(usage, "cache_creation_input_tokens");
		if (result.prompt_tokens >= 0 && result.completion_tokens >= 0) {
			result.total_tokens = result.prompt_tokens + result.completion_tokens;
		}
		YyjsonDirectString(root, "stop_reason", result.finish_reason);
		if (result.finish_reason == "max_tokens") {
			throw IOException("AI provider response stopped because max_tokens was reached; raise max_tokens or "
			                  "request a shorter response");
		}
		return result;
	}
	if (config.protocol == "ollama_chat") {
		result.prompt_tokens = YyjsonIntegerOrMissing(root, "prompt_eval_count");
		result.completion_tokens = YyjsonIntegerOrMissing(root, "eval_count");
		if (result.prompt_tokens >= 0 && result.completion_tokens >= 0) {
			result.total_tokens = result.prompt_tokens + result.completion_tokens;
		}
		YyjsonDirectString(root, "done_reason", result.finish_reason);
		return result;
	}
	auto usage = YyjsonObjectGet(root, "usage");
	result.prompt_tokens = YyjsonIntegerOrMissing(usage, "prompt_tokens");
	result.completion_tokens = YyjsonIntegerOrMissing(usage, "completion_tokens");
	result.total_tokens = YyjsonIntegerOrMissing(usage, "total_tokens");
	auto prompt_token_details = YyjsonObjectGet(usage, "prompt_tokens_details");
	result.cached_prompt_tokens = YyjsonIntegerOrMissing(prompt_token_details, "cached_tokens");
	auto first_choice = YyjsonArrayGet(YyjsonObjectGet(root, "choices"), 0);
	YyjsonDirectString(first_choice, "finish_reason", result.finish_reason);
	if (result.finish_reason == "length") {
		throw IOException("AI provider response stopped because max_tokens was reached; raise max_tokens or request a "
		                  "shorter response");
	}
	return result;
}

double EstimateCompletionCostUsd(const ProviderConfig &config, const CompletionOptions &options,
                                 const CompletionResult &result) {
	if (result.cache_hit) {
		return 0;
	}
	auto has_input_price = options.has_input_token_price_per_million;
	auto input_price = options.input_token_price_per_million;
	auto has_output_price = options.has_output_token_price_per_million;
	auto output_price = options.output_token_price_per_million;
	if (BuiltinModelPricingEnabled(options)) {
		auto price = FindBuiltinModelPrice(config.provider, config.model, "completion");
		if (price) {
			if (!has_input_price) {
				input_price = price->input_token_price_per_million;
				has_input_price = input_price >= 0;
			}
			if (!has_output_price) {
				output_price = price->output_token_price_per_million;
				has_output_price = output_price >= 0;
			}
		}
	}
	if (!has_input_price || !has_output_price || result.prompt_tokens < 0 || result.completion_tokens < 0) {
		return -1;
	}
	auto cached_prompt_tokens = std::max<int64_t>(0, result.cached_prompt_tokens);
	auto cache_creation_prompt_tokens = std::max<int64_t>(0, result.cache_creation_prompt_tokens);
	auto billable_prompt_tokens =
	    std::max<int64_t>(0, result.prompt_tokens - cached_prompt_tokens - cache_creation_prompt_tokens);
	auto cached_input_multiplier = config.protocol == "anthropic_messages" ? 0.1 : 0.5;
	auto cache_creation_multiplier = config.protocol == "anthropic_messages" ? 1.25 : 1.0;
	auto input_cost = static_cast<double>(billable_prompt_tokens) * input_price;
	input_cost += static_cast<double>(cached_prompt_tokens) * input_price * cached_input_multiplier;
	input_cost += static_cast<double>(cache_creation_prompt_tokens) * input_price * cache_creation_multiplier;
	auto output_cost = static_cast<double>(result.completion_tokens) * output_price;
	auto cost = (input_cost + output_cost) / 1000000.0;
	return HasEstimatedCost(cost) ? cost : -1;
}

double EstimateEmbeddingCostUsd(const ProviderConfig &config, const CompletionOptions &options,
                                const EmbeddingResult &result) {
	if (result.cache_hit) {
		return 0;
	}
	auto has_input_price = options.has_input_token_price_per_million;
	auto input_price = options.input_token_price_per_million;
	if (BuiltinModelPricingEnabled(options)) {
		auto price = FindBuiltinModelPrice(config.provider, config.model, "embedding");
		if (price && !has_input_price) {
			input_price = price->input_token_price_per_million;
			has_input_price = input_price >= 0;
		}
	}
	if (!has_input_price || result.prompt_tokens < 0) {
		return -1;
	}
	auto cost = static_cast<double>(result.prompt_tokens) * input_price / 1000000.0;
	return HasEstimatedCost(cost) ? cost : -1;
}

void PushUsageEvent(ProviderRuntimeState &state, UsageEvent event) {
	std::lock_guard<std::mutex> lock(state.usage_mutex);
	event.event_id = state.next_usage_event_id++;
	state.usage_events.push_back(std::move(event));
	if (state.usage_events.size() > MAX_USAGE_EVENTS) {
		state.usage_events.erase(state.usage_events.begin(),
		                         state.usage_events.begin() + (state.usage_events.size() - MAX_USAGE_EVENTS));
	}
}

void RecordUsageEvent(const ProviderConfig &config, const std::string &prompt, const CompletionResult &result,
                      const CompletionOptions &options) {
	UsageEvent event;
	event.created_at = CurrentTimestamp();
	event.event = "ai_completion";
	event.function_name = options.function_name;
	event.query_id = options.query_id;
	event.provider = config.provider;
	event.protocol = config.protocol;
	event.model = config.model;
	event.prompt_chars = static_cast<int64_t>(prompt.size());
	event.response_chars = static_cast<int64_t>(result.text.size());
	event.input_chars = -1;
	event.dimensions = -1;
	event.prompt_tokens = result.prompt_tokens;
	event.completion_tokens = result.completion_tokens;
	event.total_tokens = result.total_tokens;
	event.cached_prompt_tokens = result.cached_prompt_tokens;
	event.cache_creation_prompt_tokens = result.cache_creation_prompt_tokens;
	event.elapsed_ms = result.elapsed_ms;
	event.retries = result.retries;
	event.http_status = result.http_status;
	event.cache_hit = result.cache_hit;
	event.status = "ok";
	event.error = "";
	event.estimated_cost_usd = EstimateCompletionCostUsd(config, options, result);
	PushUsageEvent(RuntimeState(options), std::move(event));
}

void RecordFailedUsageEvent(const ProviderConfig &config, const std::string &prompt, const CompletionOptions &options,
                            long http_status, const std::string &error, int64_t retries) {
	UsageEvent event;
	event.created_at = CurrentTimestamp();
	event.event = "ai_completion";
	event.function_name = options.function_name;
	event.query_id = options.query_id;
	event.provider = config.provider;
	event.protocol = config.protocol;
	event.model = config.model;
	event.prompt_chars = static_cast<int64_t>(prompt.size());
	event.response_chars = 0;
	event.input_chars = -1;
	event.dimensions = -1;
	event.prompt_tokens = -1;
	event.completion_tokens = -1;
	event.total_tokens = -1;
	event.cached_prompt_tokens = -1;
	event.cache_creation_prompt_tokens = -1;
	event.elapsed_ms = -1;
	event.retries = retries;
	event.http_status = http_status;
	event.cache_hit = false;
	event.status = "error";
	event.error = error;
	event.estimated_cost_usd = -1;
	PushUsageEvent(RuntimeState(options), std::move(event));
}

void RecordEmbeddingUsageEvent(const ProviderConfig &config, const std::string &input, const EmbeddingResult &result,
                               const CompletionOptions &options) {
	UsageEvent event;
	event.created_at = CurrentTimestamp();
	event.event = "ai_embedding";
	event.function_name = options.function_name;
	event.query_id = options.query_id;
	event.provider = config.provider;
	event.protocol = config.protocol;
	event.model = config.model;
	event.prompt_chars = static_cast<int64_t>(input.size());
	event.response_chars = 0;
	event.input_chars = static_cast<int64_t>(input.size());
	event.dimensions = static_cast<int64_t>(result.values.size());
	event.prompt_tokens = result.prompt_tokens;
	event.completion_tokens = -1;
	event.total_tokens = result.total_tokens;
	event.cached_prompt_tokens = -1;
	event.cache_creation_prompt_tokens = -1;
	event.elapsed_ms = result.elapsed_ms;
	event.retries = result.retries;
	event.http_status = result.http_status;
	event.cache_hit = result.cache_hit;
	event.status = "ok";
	event.error = "";
	event.estimated_cost_usd = EstimateEmbeddingCostUsd(config, options, result);
	PushUsageEvent(RuntimeState(options), std::move(event));
}

void RecordFailedEmbeddingUsageEvent(const ProviderConfig &config, const std::string &input,
                                     const CompletionOptions &options, long http_status, const std::string &error,
                                     int64_t retries) {
	UsageEvent event;
	event.created_at = CurrentTimestamp();
	event.event = "ai_embedding";
	event.function_name = options.function_name;
	event.query_id = options.query_id;
	event.provider = config.provider;
	event.protocol = config.protocol;
	event.model = config.model;
	event.prompt_chars = static_cast<int64_t>(input.size());
	event.response_chars = 0;
	event.input_chars = static_cast<int64_t>(input.size());
	event.dimensions = -1;
	event.prompt_tokens = -1;
	event.completion_tokens = -1;
	event.total_tokens = -1;
	event.cached_prompt_tokens = -1;
	event.cache_creation_prompt_tokens = -1;
	event.elapsed_ms = -1;
	event.retries = retries;
	event.http_status = http_status;
	event.cache_hit = false;
	event.status = "error";
	event.error = error;
	event.estimated_cost_usd = -1;
	PushUsageEvent(RuntimeState(options), std::move(event));
}

double LogSampleRate(const CompletionOptions &options) {
	if (options.has_log_sample_rate) {
		if (!std::isfinite(options.log_sample_rate) || options.log_sample_rate < 0 || options.log_sample_rate > 1) {
			throw InvalidInputException("DUCKDB_AI_LOG_SAMPLE_RATE must be between 0 and 1");
		}
		return options.log_sample_rate;
	}
	auto configured = GetEnv("DUCKDB_AI_LOG_SAMPLE_RATE");
	if (configured.empty()) {
		return 1.0;
	}
	try {
		auto rate = std::stod(configured);
		if (!std::isfinite(rate) || rate < 0 || rate > 1) {
			throw InvalidInputException("DUCKDB_AI_LOG_SAMPLE_RATE must be between 0 and 1");
		}
		return rate;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_LOG_SAMPLE_RATE must be a number between 0 and 1");
	}
}

uint64_t StableHash(const std::string &input) {
	uint64_t hash = 1469598103934665603ULL;
	for (auto c : input) {
		hash ^= static_cast<unsigned char>(c);
		hash *= 1099511628211ULL;
	}
	return hash;
}

bool ShouldPostLog(const CompletionOptions &options, const std::string &sample_key) {
	auto rate = LogSampleRate(options);
	if (rate >= 1) {
		return true;
	}
	if (rate <= 0) {
		return false;
	}
	auto bucket = static_cast<double>(StableHash(sample_key) % 1000000) / 1000000.0;
	return bucket < rate;
}

std::string LogTags(const CompletionOptions &options) {
	return options.log_tags.empty() ? GetEnv("DUCKDB_AI_LOG_TAGS") : options.log_tags;
}

std::string LogFormat(const CompletionOptions &options) {
	auto format = LowerAscii(options.log_format.empty() ? GetEnv("DUCKDB_AI_LOG_FORMAT") : options.log_format);
	if (format.empty() || format == "generic" || format == "json" || format == "generic_json") {
		return "generic_json";
	}
	if (format == "otlp" || format == "otlp_json") {
		return "otlp_json";
	}
	throw InvalidInputException("DUCKDB_AI_LOG_FORMAT must be one of: generic, json, generic_json, otlp, otlp_json");
}

bool LogStrict(const CompletionOptions &options) {
	return options.has_log_strict ? options.log_strict : EnvFlagEnabled("DUCKDB_AI_LOG_STRICT");
}

void SubmitUsageLog(const CompletionOptions &options, const std::string &log_endpoint, std::string payload,
                    const std::string &error_prefix) {
	auto log_strict = LogStrict(options);
	try {
		EnforceAllowedHost(log_endpoint, options);
		if (log_strict) {
			HttpPost(log_endpoint, payload, {"Content-Type: application/json"}, TimeoutSeconds(options), true,
			         RetryCount(options), RetryBackoffMs(options), options.client_context);
			return;
		}
		UsageLogJob job;
		job.url = log_endpoint;
		job.payload = std::move(payload);
		job.headers = {"Content-Type: application/json"};
		job.timeout_seconds = TimeoutSeconds(options);
		job.retry_count = RetryCount(options);
		job.retry_backoff_ms = RetryBackoffMs(options);
		EnqueueUsageLog(RuntimeState(options), std::move(job));
	} catch (std::exception &ex) {
		if (log_strict) {
			throw IOException("%s: %s", error_prefix, ex.what());
		}
	}
}

void AppendJsonAttribute(std::string &payload, bool &wrote, const std::string &attribute) {
	if (wrote) {
		payload += ",";
	}
	payload += attribute;
	wrote = true;
}

std::string OtlpStringAttribute(const std::string &key, const std::string &value) {
	return "{\"key\":\"" + JsonEscape(key) + "\",\"value\":{\"stringValue\":\"" + JsonEscape(value) + "\"}}";
}

std::string OtlpIntAttribute(const std::string &key, int64_t value) {
	return "{\"key\":\"" + JsonEscape(key) + "\",\"value\":{\"intValue\":\"" + std::to_string(value) + "\"}}";
}

std::string OtlpDoubleAttribute(const std::string &key, double value) {
	return "{\"key\":\"" + JsonEscape(key) + "\",\"value\":{\"doubleValue\":" + JsonDouble(value) + "}}";
}

std::string OtlpLogPayload(const std::string &event, const std::string &function_name, const std::string &query_id,
                           const std::string &provider, const std::string &protocol, const std::string &model,
                           const std::string &log_tags, const std::string &text_name, const std::string &text_value,
                           const std::string &extra_text_name, const std::string &extra_text_value, bool include_text,
                           const std::vector<std::pair<std::string, int64_t>> &metrics, double estimated_cost_usd) {
	std::string attributes;
	bool wrote_attribute = false;
	AppendJsonAttribute(attributes, wrote_attribute, OtlpStringAttribute("ai.event", event));
	if (!function_name.empty()) {
		AppendJsonAttribute(attributes, wrote_attribute, OtlpStringAttribute("ai.function_name", function_name));
	}
	if (!query_id.empty()) {
		AppendJsonAttribute(attributes, wrote_attribute, OtlpStringAttribute("ai.query_id", query_id));
	}
	AppendJsonAttribute(attributes, wrote_attribute, OtlpStringAttribute("ai.provider", provider));
	AppendJsonAttribute(attributes, wrote_attribute, OtlpStringAttribute("ai.protocol", protocol));
	AppendJsonAttribute(attributes, wrote_attribute, OtlpStringAttribute("ai.model", model));
	for (auto &metric : metrics) {
		AppendJsonAttribute(attributes, wrote_attribute, OtlpIntAttribute(metric.first, metric.second));
	}
	if (HasEstimatedCost(estimated_cost_usd)) {
		AppendJsonAttribute(attributes, wrote_attribute,
		                    OtlpDoubleAttribute("ai.estimated_cost_usd", estimated_cost_usd));
	}
	if (!log_tags.empty()) {
		AppendJsonAttribute(attributes, wrote_attribute, OtlpStringAttribute("ai.tags", log_tags));
	}
	if (include_text) {
		AppendJsonAttribute(attributes, wrote_attribute, OtlpStringAttribute(text_name, text_value));
		if (!extra_text_name.empty()) {
			AppendJsonAttribute(attributes, wrote_attribute, OtlpStringAttribute(extra_text_name, extra_text_value));
		}
	}

	return std::string("{\"resourceLogs\":[{\"resource\":{\"attributes\":[") +
	       OtlpStringAttribute("service.name", "duckdb_ai") +
	       "]},\"scopeLogs\":[{\"scope\":{\"name\":\"duckdb_ai\"},\"logRecords\":[{\"severityText\":\"INFO\","
	       "\"body\":{\"stringValue\":\"" +
	       JsonEscape(event) + "\"},\"attributes\":[" + attributes + "]}]}]}]}";
}

void MaybePostUsageLog(const ProviderConfig &config, const std::string &prompt, const CompletionResult &result,
                       const CompletionOptions &options) {
	auto log_endpoint = options.log_endpoint.empty() ? GetEnv("DUCKDB_AI_LOG_ENDPOINT") : options.log_endpoint;
	if (log_endpoint.empty()) {
		return;
	}
	if (!ShouldPostLog(options, "completion|" + config.provider + "|" + config.model + "|" + prompt)) {
		return;
	}

	auto include_text =
	    options.has_log_include_text ? options.log_include_text : EnvFlagEnabled("DUCKDB_AI_LOG_INCLUDE_TEXT");
	auto log_tags = LogTags(options);
	auto estimated_cost_usd = EstimateCompletionCostUsd(config, options, result);
	std::string payload;
	if (LogFormat(options) == "otlp_json") {
		std::vector<std::pair<std::string, int64_t>> metrics {
		    {"ai.prompt_chars", static_cast<int64_t>(prompt.size())},
		    {"ai.response_chars", static_cast<int64_t>(result.text.size())},
		    {"ai.prompt_tokens", result.prompt_tokens},
		    {"ai.completion_tokens", result.completion_tokens},
		    {"ai.total_tokens", result.total_tokens},
		    {"ai.cached_prompt_tokens", result.cached_prompt_tokens},
		    {"ai.cache_creation_prompt_tokens", result.cache_creation_prompt_tokens},
		    {"ai.elapsed_ms", result.elapsed_ms},
		    {"ai.retries", result.retries},
		    {"http.status_code", result.http_status},
		};
		payload = OtlpLogPayload("ai_completion", options.function_name, options.query_id, config.provider,
		                         config.protocol, config.model, log_tags, "ai.prompt", prompt, "ai.response",
		                         result.text, include_text, metrics, estimated_cost_usd);
	} else {
		payload = std::string("{") + "\"extension\":\"duckdb_ai\"," + "\"event\":\"ai_completion\"," +
		          "\"function_name\":\"" + JsonEscape(options.function_name) + "\"," + "\"query_id\":\"" +
		          JsonEscape(options.query_id) + "\"," + "\"provider\":\"" + JsonEscape(config.provider) + "\"," +
		          "\"protocol\":\"" + JsonEscape(config.protocol) + "\"," + "\"model\":\"" + JsonEscape(config.model) +
		          "\"," + "\"prompt_chars\":" + std::to_string(prompt.size()) + "," +
		          "\"response_chars\":" + std::to_string(result.text.size()) + "," +
		          "\"prompt_tokens\":" + std::to_string(result.prompt_tokens) + "," +
		          "\"completion_tokens\":" + std::to_string(result.completion_tokens) + "," +
		          "\"total_tokens\":" + std::to_string(result.total_tokens) + "," +
		          "\"cached_prompt_tokens\":" + std::to_string(result.cached_prompt_tokens) + "," +
		          "\"cache_creation_prompt_tokens\":" + std::to_string(result.cache_creation_prompt_tokens) + "," +
		          "\"elapsed_ms\":" + std::to_string(result.elapsed_ms) + "," +
		          "\"retries\":" + std::to_string(result.retries) + "," +
		          "\"http_status\":" + std::to_string(result.http_status) + "," +
		          "\"cache_hit\":" + std::string(result.cache_hit ? "true" : "false") + "," + "\"status\":\"ok\"";

		if (HasEstimatedCost(estimated_cost_usd)) {
			payload += ",\"estimated_cost_usd\":" + JsonDouble(estimated_cost_usd);
		}
		if (include_text) {
			payload += ",\"prompt\":\"" + JsonEscape(prompt) + "\",\"response\":\"" + JsonEscape(result.text) + "\"";
		}
		if (!log_tags.empty()) {
			payload += ",\"tags\":\"" + JsonEscape(log_tags) + "\"";
		}
		payload += "}";
	}

	SubmitUsageLog(options, log_endpoint, std::move(payload), "AI usage log request failed");
}

void MaybePostEmbeddingLog(const ProviderConfig &config, const std::string &input, const EmbeddingResult &result,
                           const CompletionOptions &options) {
	auto log_endpoint = options.log_endpoint.empty() ? GetEnv("DUCKDB_AI_LOG_ENDPOINT") : options.log_endpoint;
	if (log_endpoint.empty()) {
		return;
	}
	if (!ShouldPostLog(options, "embedding|" + config.provider + "|" + config.model + "|" + input)) {
		return;
	}

	auto include_text =
	    options.has_log_include_text ? options.log_include_text : EnvFlagEnabled("DUCKDB_AI_LOG_INCLUDE_TEXT");
	auto log_tags = LogTags(options);
	auto estimated_cost_usd = EstimateEmbeddingCostUsd(config, options, result);
	std::string payload;
	if (LogFormat(options) == "otlp_json") {
		std::vector<std::pair<std::string, int64_t>> metrics {
		    {"ai.input_chars", static_cast<int64_t>(input.size())},
		    {"ai.dimensions", static_cast<int64_t>(result.values.size())},
		    {"ai.prompt_tokens", result.prompt_tokens},
		    {"ai.total_tokens", result.total_tokens},
		    {"ai.elapsed_ms", result.elapsed_ms},
		    {"ai.retries", result.retries},
		    {"http.status_code", result.http_status},
		};
		payload = OtlpLogPayload("ai_embedding", options.function_name, options.query_id, config.provider,
		                         config.protocol, config.model, log_tags, "ai.input", input, "", "", include_text,
		                         metrics, estimated_cost_usd);
	} else {
		payload = std::string("{") + "\"extension\":\"duckdb_ai\"," + "\"event\":\"ai_embedding\"," +
		          "\"function_name\":\"" + JsonEscape(options.function_name) + "\"," + "\"query_id\":\"" +
		          JsonEscape(options.query_id) + "\"," + "\"provider\":\"" + JsonEscape(config.provider) + "\"," +
		          "\"protocol\":\"" + JsonEscape(config.protocol) + "\"," + "\"model\":\"" + JsonEscape(config.model) +
		          "\"," + "\"input_chars\":" + std::to_string(input.size()) + "," +
		          "\"dimensions\":" + std::to_string(result.values.size()) + "," +
		          "\"prompt_tokens\":" + std::to_string(result.prompt_tokens) + "," +
		          "\"total_tokens\":" + std::to_string(result.total_tokens) + "," +
		          "\"elapsed_ms\":" + std::to_string(result.elapsed_ms) + "," +
		          "\"retries\":" + std::to_string(result.retries) + "," +
		          "\"http_status\":" + std::to_string(result.http_status) + "," +
		          "\"cache_hit\":" + std::string(result.cache_hit ? "true" : "false") + "," + "\"status\":\"ok\"";

		if (HasEstimatedCost(estimated_cost_usd)) {
			payload += ",\"estimated_cost_usd\":" + JsonDouble(estimated_cost_usd);
		}
		if (include_text) {
			payload += ",\"input\":\"" + JsonEscape(input) + "\"";
		}
		if (!log_tags.empty()) {
			payload += ",\"tags\":\"" + JsonEscape(log_tags) + "\"";
		}
		payload += "}";
	}

	SubmitUsageLog(options, log_endpoint, std::move(payload), "AI embedding log request failed");
}

//! Serialize one embedding into a minimal single-input response body so batched results can be
//! stored in the response cache and replayed later through ParseEmbeddingResult.
std::string SerializeEmbeddingCacheBody(const ProviderConfig &config, const EmbeddingResult &result) {
	std::string body = config.protocol == "ollama_embed" ? "{\"embeddings\":[[" : "{\"data\":[{\"embedding\":[";
	for (idx_t i = 0; i < result.values.size(); i++) {
		if (i > 0) {
			body += ",";
		}
		body += JsonDouble(result.values[i]);
	}
	body += config.protocol == "ollama_embed" ? "]]" : "]}]";
	if (result.prompt_tokens >= 0) {
		body += ",\"prompt_tokens\":" + std::to_string(result.prompt_tokens);
	}
	if (result.total_tokens >= 0) {
		body += ",\"total_tokens\":" + std::to_string(result.total_tokens);
	}
	body += "}";
	return body;
}

//! Fetch a provider response, going through the response cache and in-flight request coalescing
//! when caching is enabled. Sets cache_key_out whenever the response cache is consulted so
//! callers can evict entries that later fail response parsing.
HttpResponse FetchProviderResponse(const std::string &operation, const ProviderConfig &config,
                                   const std::string &endpoint, const std::string &payload,
                                   const CompletionOptions &options, int64_t estimated_tokens,
                                   std::string &cache_key_out) {
	if (!ResponseCacheEnabled(options)) {
		return ProviderHttpPost(endpoint, payload, RequestHeaders(config), options, estimated_tokens);
	}
	auto &runtime_state = RuntimeState(options);
	auto cache_key = ResponseCacheKey(operation, config, endpoint, payload);
	cache_key_out = cache_key;
	HttpResponse cached_response;
	if (TryGetCachedResponse(runtime_state, cache_key, options, cached_response)) {
		return cached_response;
	}
	bool owns_request = false;
	auto pending = BeginPendingCachedResponse(runtime_state, cache_key, owns_request);
	if (!owns_request) {
		return WaitForPendingCachedResponse(runtime_state, cache_key, pending, options);
	}
	try {
		auto fresh_response = ProviderHttpPost(endpoint, payload, RequestHeaders(config), options, estimated_tokens);
		if (fresh_response.status >= 200 && fresh_response.status < 300) {
			StoreCachedResponse(runtime_state, cache_key, fresh_response, options);
		}
		FinishPendingCachedResponse(runtime_state, cache_key, pending, fresh_response);
		return fresh_response;
	} catch (std::exception &ex) {
		FailPendingCachedResponse(runtime_state, cache_key, pending, ex.what());
		throw;
	} catch (...) {
		FailPendingCachedResponse(runtime_state, cache_key, pending, "unknown AI provider error");
		throw;
	}
}

//! Issue one batched embedding request and record per-input usage events.
std::vector<EmbeddingResult> EmbedBatchRequest(const ProviderConfig &config, const std::string &endpoint,
                                               const std::vector<std::string> &inputs,
                                               const CompletionOptions &options) {
	auto payload = EmbeddingPayload(config, inputs);
	auto total_tokens = int64_t(0);
	for (auto &input : inputs) {
		total_tokens += EstimateTokenCount(input);
	}
	HttpResponse response;
	try {
		response = ProviderHttpPost(endpoint, payload, RequestHeaders(config), options, total_tokens);
	} catch (InterruptException &) {
		throw;
	} catch (std::exception &ex) {
		for (auto &input : inputs) {
			RecordFailedEmbeddingUsageEvent(config, input, options, 0, ex.what(), 0);
		}
		throw;
	}
	if (response.status < 200 || response.status >= 300) {
		auto error = ProviderErrorDetail(config, response);
		for (auto &input : inputs) {
			RecordFailedEmbeddingUsageEvent(config, input, options, response.status, error, response.retries);
		}
		throw IOException("%s", error);
	}
	std::vector<EmbeddingResult> results;
	try {
		results = ParseEmbeddingResults(config, response, inputs.size());
	} catch (std::exception &ex) {
		for (auto &input : inputs) {
			RecordFailedEmbeddingUsageEvent(config, input, options, response.status, ex.what(), response.retries);
		}
		throw;
	}
	for (idx_t i = 0; i < results.size(); i++) {
		RecordEmbeddingUsageEvent(config, inputs[i], results[i], options);
		MaybePostEmbeddingLog(config, inputs[i], results[i], options);
	}
	return results;
}

} // namespace

std::string NormalizeProviderName(const std::string &provider) {
	return NormalizeProviderNameInternal(provider);
}

ProviderConfig ResolveProvider(const std::string &provider, const std::string &model) {
	CompletionOptions options;
	options.provider = provider;
	options.model = model;
	return ResolveProvider(options);
}

ProviderConfig ResolveProvider(const CompletionOptions &options) {
	return ResolveProviderConfig(options, true);
}

void SnapshotEnvironmentOptions(CompletionOptions &options) {
	SnapshotEnvironmentOptionsInternal(options);
}

std::string ProviderBaseUrl(const std::string &provider) {
	return ResolveBaseUrl(ProviderDefaults(provider));
}

std::string ProviderProtocol(const std::string &provider) {
	return ProviderDefaults(provider).protocol;
}

int64_t EstimateTokenCount(const std::string &input) {
	if (input.empty()) {
		return 0;
	}
	return static_cast<int64_t>((input.size() + 3) / 4);
}

std::string BuildRequestJson(const std::string &prompt, const std::string &model, const std::string &provider) {
	CompletionOptions options;
	options.model = model;
	options.provider = provider;
	return BuildRequestJson(prompt, options);
}

std::string BuildRequestJson(const std::string &prompt, const CompletionOptions &options) {
	auto config = ResolveProviderConfig(options, false);
	return RequestPayload(config, prompt, options);
}

CompletionResult Complete(const std::string &prompt, const std::string &model, const std::string &provider) {
	CompletionOptions options;
	options.model = model;
	options.provider = provider;
	return Complete(prompt, options);
}

CompletionResult Complete(const std::string &prompt, const CompletionOptions &options) {
	if (prompt.empty()) {
		throw InvalidInputException("ai_complete prompt must not be empty");
	}
	auto config = ResolveProvider(options);
	auto endpoint = RequestEndpoint(config);
	auto payload = RequestPayload(config, prompt, options);
	EnforceAllowedHost(endpoint, options);
	HttpResponse response;
	std::string cache_key;
	try {
		response = FetchProviderResponse("completion", config, endpoint, payload, options,
		                                 EstimatedCompletionTokens(prompt, options), cache_key);
	} catch (InterruptException &) {
		// Query cancellation is not a provider failure; keep it out of usage error events.
		throw;
	} catch (std::exception &ex) {
		RecordFailedUsageEvent(config, prompt, options, 0, ex.what(), 0);
		throw;
	}
	if (response.status < 200 || response.status >= 300) {
		auto error = ProviderErrorDetail(config, response);
		RecordFailedUsageEvent(config, prompt, options, response.status, error, response.retries);
		throw IOException("%s", error);
	}
	CompletionResult result;
	try {
		result = ParseCompletionResult(config, response);
	} catch (std::exception &ex) {
		if (!cache_key.empty()) {
			// Do not keep responses that cannot be parsed (for example truncated output); a
			// poisoned entry would fail every later cache hit until ai_clear_cache().
			RemoveCachedResponse(RuntimeState(options), cache_key);
		}
		RecordFailedUsageEvent(config, prompt, options, response.status, ex.what(), response.retries);
		throw;
	}
	RecordUsageEvent(config, prompt, result, options);
	MaybePostUsageLog(config, prompt, result, options);
	return result;
}

CompletionResult Redact(const std::string &text, const CompletionOptions &options) {
	if (text.empty()) {
		throw InvalidInputException("ai_redact text must not be empty");
	}
	auto config = ResolveProvider(options);
	if (config.protocol != "privacy_filter") {
		throw InvalidInputException("AI provider \"%s\" does not support dedicated Privacy Filter redaction.",
		                            config.provider);
	}
	auto endpoint = RequestEndpoint(config);
	auto payload = RequestPayload(config, text, options);
	EnforceAllowedHost(endpoint, options);
	HttpResponse response;
	std::string cache_key;
	try {
		response =
		    FetchProviderResponse("redaction", config, endpoint, payload, options, EstimateTokenCount(text), cache_key);
	} catch (InterruptException &) {
		throw;
	} catch (std::exception &ex) {
		RecordFailedUsageEvent(config, text, options, 0, ex.what(), 0);
		throw;
	}
	if (response.status < 200 || response.status >= 300) {
		auto error = ProviderErrorDetail(config, response);
		RecordFailedUsageEvent(config, text, options, response.status, error, response.retries);
		throw IOException("%s", error);
	}
	CompletionResult result;
	try {
		result = ParseCompletionResult(config, response);
	} catch (std::exception &ex) {
		if (!cache_key.empty()) {
			RemoveCachedResponse(RuntimeState(options), cache_key);
		}
		RecordFailedUsageEvent(config, text, options, response.status, ex.what(), response.retries);
		throw;
	}
	RecordUsageEvent(config, text, result, options);
	MaybePostUsageLog(config, text, result, options);
	return result;
}

std::string BuildEmbeddingRequestJson(const std::string &input, const std::string &model, const std::string &provider) {
	CompletionOptions options;
	options.model = model;
	options.provider = provider;
	return BuildEmbeddingRequestJson(input, options);
}

std::string BuildEmbeddingRequestJson(const std::string &input, const CompletionOptions &options) {
	auto config = ResolveEmbeddingProviderConfig(options, false);
	return EmbeddingPayload(config, input);
}

EmbeddingResult Embed(const std::string &input, const std::string &model, const std::string &provider) {
	CompletionOptions options;
	options.model = model;
	options.provider = provider;
	return Embed(input, options);
}

EmbeddingResult Embed(const std::string &input, const CompletionOptions &options) {
	if (input.empty()) {
		throw InvalidInputException("ai_embed input must not be empty");
	}
	auto config = ResolveEmbeddingProviderConfig(options, true);
	auto endpoint = EmbeddingEndpoint(config);
	auto payload = EmbeddingPayload(config, input);
	EnforceAllowedHost(endpoint, options);
	HttpResponse response;
	std::string cache_key;
	try {
		response = FetchProviderResponse("embedding", config, endpoint, payload, options, EstimateTokenCount(input),
		                                 cache_key);
	} catch (InterruptException &) {
		throw;
	} catch (std::exception &ex) {
		RecordFailedEmbeddingUsageEvent(config, input, options, 0, ex.what(), 0);
		throw;
	}
	if (response.status < 200 || response.status >= 300) {
		auto error = ProviderErrorDetail(config, response);
		RecordFailedEmbeddingUsageEvent(config, input, options, response.status, error, response.retries);
		throw IOException("%s", error);
	}
	EmbeddingResult result;
	try {
		result = ParseEmbeddingResult(config, response);
	} catch (std::exception &ex) {
		if (!cache_key.empty()) {
			RemoveCachedResponse(RuntimeState(options), cache_key);
		}
		RecordFailedEmbeddingUsageEvent(config, input, options, response.status, ex.what(), response.retries);
		throw;
	}
	RecordEmbeddingUsageEvent(config, input, result, options);
	MaybePostEmbeddingLog(config, input, result, options);
	return result;
}

std::vector<EmbeddingResult> EmbedMany(const std::vector<std::string> &inputs, const CompletionOptions &options) {
	if (inputs.empty()) {
		return {};
	}
	if (inputs.size() == 1) {
		return {Embed(inputs[0], options)};
	}
	for (auto &input : inputs) {
		if (input.empty()) {
			throw InvalidInputException("ai_embed input must not be empty");
		}
	}
	auto config = ResolveEmbeddingProviderConfig(options, true);
	auto endpoint = EmbeddingEndpoint(config);
	EnforceAllowedHost(endpoint, options);

	std::vector<EmbeddingResult> results(inputs.size());
	std::vector<idx_t> miss_rows;
	miss_rows.reserve(inputs.size());
	auto cache_enabled = ResponseCacheEnabled(options);
	auto &runtime_state = RuntimeState(options);
	if (cache_enabled) {
		// Serve cached inputs individually and batch only the misses, so enabling the response
		// cache does not disable batched embedding requests.
		for (idx_t i = 0; i < inputs.size(); i++) {
			auto cache_key = ResponseCacheKey("embedding", config, endpoint, EmbeddingPayload(config, inputs[i]));
			HttpResponse cached_response;
			if (TryGetCachedResponse(runtime_state, cache_key, options, cached_response)) {
				try {
					results[i] = ParseEmbeddingResult(config, cached_response);
				} catch (...) {
					RemoveCachedResponse(runtime_state, cache_key);
					throw;
				}
				RecordEmbeddingUsageEvent(config, inputs[i], results[i], options);
				MaybePostEmbeddingLog(config, inputs[i], results[i], options);
				continue;
			}
			miss_rows.push_back(i);
		}
	} else {
		for (idx_t i = 0; i < inputs.size(); i++) {
			miss_rows.push_back(i);
		}
	}

	for (idx_t batch_start = 0; batch_start < miss_rows.size(); batch_start += MAX_EMBED_BATCH_INPUTS) {
		auto batch_end = std::min<idx_t>(batch_start + MAX_EMBED_BATCH_INPUTS, miss_rows.size());
		std::vector<std::string> batch_inputs;
		batch_inputs.reserve(batch_end - batch_start);
		for (idx_t i = batch_start; i < batch_end; i++) {
			batch_inputs.push_back(inputs[miss_rows[i]]);
		}
		auto batch_results = batch_inputs.size() == 1 ? std::vector<EmbeddingResult> {Embed(batch_inputs[0], options)}
		                                              : EmbedBatchRequest(config, endpoint, batch_inputs, options);
		for (idx_t i = 0; i < batch_results.size(); i++) {
			auto input_row = miss_rows[batch_start + i];
			if (cache_enabled && batch_inputs.size() > 1) {
				auto cache_key =
				    ResponseCacheKey("embedding", config, endpoint, EmbeddingPayload(config, inputs[input_row]));
				HttpResponse cache_entry(SerializeEmbeddingCacheBody(config, batch_results[i]),
				                         batch_results[i].http_status, 0, -1);
				StoreCachedResponse(runtime_state, cache_key, cache_entry, options);
			}
			results[input_row] = std::move(batch_results[i]);
		}
	}
	return results;
}

void InitializeProviderRuntime() {
	EnsureCurlGlobalInit();
}

void AttachProviderRuntimeState(CompletionOptions &options, ClientContext &context) {
	options.client_context = &context;
	options.runtime_state = &RuntimeState(context);
}

int64_t EffectiveMaxConcurrentRequests(const CompletionOptions &options) {
	return MaxConcurrentRequests(options);
}

std::vector<UsageEvent> UsageEvents() {
	std::lock_guard<std::mutex> lock(fallback_runtime_state.usage_mutex);
	return fallback_runtime_state.usage_events;
}

std::vector<UsageEvent> UsageEvents(ClientContext &context) {
	auto &state = RuntimeState(context);
	std::lock_guard<std::mutex> lock(state.usage_mutex);
	return state.usage_events;
}

void ClearUsageEvents() {
	std::lock_guard<std::mutex> lock(fallback_runtime_state.usage_mutex);
	fallback_runtime_state.usage_events.clear();
}

void ClearUsageEvents(ClientContext &context) {
	auto &state = RuntimeState(context);
	std::lock_guard<std::mutex> lock(state.usage_mutex);
	state.usage_events.clear();
}

void ClearResponseCache() {
	std::lock_guard<std::mutex> lock(fallback_runtime_state.response_cache_mutex);
	fallback_runtime_state.response_cache.clear();
	fallback_runtime_state.response_cache_order.clear();
}

void ClearResponseCache(ClientContext &context) {
	auto &state = RuntimeState(context);
	std::lock_guard<std::mutex> lock(state.response_cache_mutex);
	state.response_cache.clear();
	state.response_cache_order.clear();
}

std::vector<ModelPrice> ModelPrices() {
	return BuiltinModelPrices();
}

void RecordLocalUsageEvent(ClientContext *context, const std::string &event_name, int64_t input_chars,
                           int64_t response_chars) {
	UsageEvent event;
	event.created_at = CurrentTimestamp();
	event.event = event_name;
	event.function_name = event_name;
	event.query_id = "";
	event.provider = "local";
	event.protocol = "duckdb";
	event.model = "";
	event.prompt_chars = input_chars;
	event.response_chars = response_chars;
	event.input_chars = input_chars;
	event.dimensions = -1;
	event.prompt_tokens = -1;
	event.completion_tokens = -1;
	event.total_tokens = -1;
	event.cached_prompt_tokens = -1;
	event.cache_creation_prompt_tokens = -1;
	event.elapsed_ms = 0;
	event.retries = 0;
	event.http_status = 0;
	event.cache_hit = false;
	event.status = "ok";
	event.error = "";
	event.estimated_cost_usd = -1;
	auto &state = context ? RuntimeState(*context) : fallback_runtime_state;
	PushUsageEvent(state, std::move(event));
}

void RecordLocalUsageEvent(const std::string &event_name, int64_t input_chars, int64_t response_chars) {
	RecordLocalUsageEvent(nullptr, event_name, input_chars, response_chars);
}

bool ValidateJsonDocument(const std::string &input, std::string &error) {
	return ValidateJsonDocumentInternal(input, error);
}

bool ExtractJsonSchemaProperties(const std::string &schema, std::vector<JsonSchemaProperty> &properties,
                                 std::string &error) {
	JsonValue schema_value;
	if (!ParseJsonValueDocument(schema, schema_value, error)) {
		error = "schema is not valid JSON: " + error;
		return false;
	}
	if (schema_value.type != JsonValueType::OBJECT) {
		error = "schema must be a JSON object";
		return false;
	}
	auto root_type = ObjectField(schema_value, "type");
	JsonValue object_value;
	object_value.type = JsonValueType::OBJECT;
	if (root_type && !SchemaTypeAllows(*root_type, object_value)) {
		error = "schema root type must allow object";
		return false;
	}
	auto properties_schema = ObjectField(schema_value, "properties");
	if (!properties_schema || properties_schema->type != JsonValueType::OBJECT ||
	    properties_schema->object_value.empty()) {
		error = "schema must define at least one top-level object property";
		return false;
	}
	ExtractSchemaPropertyList(schema_value, properties);
	return true;
}

JsonExtractedValue ExtractJsonValue(const JsonValue &value) {
	JsonExtractedValue extracted;
	extracted.json_value = JsonValueToJson(value);
	switch (value.type) {
	case JsonValueType::NULL_VALUE:
		extracted.kind = JsonExtractedKind::NULL_VALUE;
		break;
	case JsonValueType::BOOLEAN:
		extracted.kind = JsonExtractedKind::BOOLEAN;
		extracted.boolean_value = value.boolean_value;
		break;
	case JsonValueType::NUMBER:
		extracted.kind = JsonExtractedKind::NUMBER;
		extracted.number_value = value.number_value;
		extracted.number_is_integer = IsJsonInteger(value);
		break;
	case JsonValueType::STRING:
		extracted.kind = JsonExtractedKind::STRING;
		extracted.string_value = value.string_value;
		break;
	case JsonValueType::ARRAY:
		extracted.kind = JsonExtractedKind::ARRAY;
		for (auto &item : value.array_value) {
			auto child = ExtractJsonValue(item);
			if (item.type == JsonValueType::STRING) {
				extracted.string_array.push_back(item.string_value);
			}
			extracted.array_values.push_back(std::move(child));
		}
		if (extracted.string_array.size() != extracted.array_values.size()) {
			extracted.string_array.clear();
		}
		break;
	case JsonValueType::OBJECT:
		extracted.kind = JsonExtractedKind::OBJECT;
		for (auto &entry : value.object_value) {
			extracted.object_values.push_back(make_pair(entry.first, ExtractJsonValue(entry.second)));
		}
		break;
	}
	return extracted;
}

bool ExtractJsonObjectFields(const std::string &input, const std::vector<std::string> &field_names,
                             std::vector<JsonExtractedValue> &values, std::string &error) {
	JsonValue value;
	if (!ParseJsonValueDocument(input, value, error)) {
		return false;
	}
	if (value.type != JsonValueType::OBJECT) {
		error = "expected top-level JSON object";
		return false;
	}
	values.clear();
	values.reserve(field_names.size());
	for (auto &field_name : field_names) {
		JsonExtractedValue extracted;
		auto field = value.object_value.find(field_name);
		if (field == value.object_value.end()) {
			values.push_back(std::move(extracted));
			continue;
		}
		extracted = ExtractJsonValue(field->second);
		values.push_back(std::move(extracted));
	}
	return true;
}

bool ExtractJsonStringArray(const std::string &input, std::vector<std::string> &values, std::string &error) {
	JsonValue value;
	if (!ParseJsonValueDocument(input, value, error)) {
		return false;
	}
	if (value.type != JsonValueType::ARRAY) {
		error = "expected top-level JSON array";
		return false;
	}
	values.clear();
	values.reserve(value.array_value.size());
	for (auto &item : value.array_value) {
		if (item.type != JsonValueType::STRING) {
			error = "expected JSON array entries to be strings";
			values.clear();
			return false;
		}
		values.push_back(item.string_value);
	}
	return true;
}

bool ValidateJsonAgainstSchema(const std::string &input, const std::string &schema, std::string &error) {
	JsonValue value;
	JsonValue schema_value;
	if (!ParseJsonValueDocument(input, value, error)) {
		return false;
	}
	if (!ParseJsonValueDocument(schema, schema_value, error)) {
		error = "schema is not valid JSON: " + error;
		return false;
	}
	if (schema_value.type != JsonValueType::OBJECT) {
		error = "schema must be a JSON object";
		return false;
	}
	return ValidateJsonValueAgainstSchema(value, schema_value, "$", error);
}

} // namespace duckdb_ai
} // namespace duckdb
