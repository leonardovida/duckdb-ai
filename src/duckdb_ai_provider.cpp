#include "duckdb_ai_provider.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <curl/curl.h>
#include <exception>
#include <initializer_list>
#include <iomanip>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace duckdb {
namespace duckdb_ai {

namespace {

struct HttpResponse {
	std::string body;
	long status;
	int64_t elapsed_ms;
};

std::mutex usage_mutex;
std::vector<UsageEvent> usage_events;
uint64_t next_usage_event_id = 1;
const size_t MAX_USAGE_EVENTS = 1024;
std::mutex provider_control_mutex;
std::condition_variable provider_control_cv;
int64_t active_provider_requests = 0;
bool has_last_provider_request_start = false;
std::chrono::steady_clock::time_point last_provider_request_start;

std::string LowerAscii(std::string input) {
	std::transform(input.begin(), input.end(), input.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return input;
}

std::string NormalizeProviderNameInternal(const std::string &provider_input) {
	auto provider = LowerAscii(provider_input);
	if (provider == "anthropic") {
		return "claude";
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
	if (provider == "openai-compatible" || provider == "openai_compatible" || provider == "local" ||
	    provider == "local_openai" || provider == "local-models" || provider == "local_models") {
		return "openai_compatible";
	}
	return provider;
}

std::string GetEnv(const std::string &name) {
	const char *value = std::getenv(name.c_str());
	return value ? std::string(value) : std::string();
}

std::string TrimTrailingSlash(std::string value) {
	while (value.size() > 1 && value.back() == '/') {
		value.pop_back();
	}
	return value;
}

bool EndsWith(const std::string &value, const std::string &suffix) {
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string AzureOpenAIBaseUrl(std::string value) {
	value = TrimTrailingSlash(std::move(value));
	if (value.empty() || EndsWith(value, "/openai/v1")) {
		return value;
	}
	return value + "/openai/v1";
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
	std::ostringstream out;
	for (auto c : input) {
		switch (c) {
		case '\\':
			out << "\\\\";
			break;
		case '"':
			out << "\\\"";
			break;
		case '\b':
			out << "\\b";
			break;
		case '\f':
			out << "\\f";
			break;
		case '\n':
			out << "\\n";
			break;
		case '\r':
			out << "\\r";
			break;
		case '\t':
			out << "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				auto value = static_cast<unsigned char>(c);
				out << "\\u";
				const char *hex = "0123456789abcdef";
				out << "00" << hex[(value >> 4) & 0xf] << hex[value & 0xf];
			} else {
				out << c;
			}
		}
	}
	return out.str();
}

std::string JsonDouble(double input) {
	std::ostringstream out;
	out << std::setprecision(15) << input;
	return out.str();
}

bool HasEstimatedCost(double estimated_cost_usd) {
	return estimated_cost_usd >= 0 && std::isfinite(estimated_cost_usd);
}

const std::vector<ModelPrice> &BuiltinModelPrices() {
	static const std::vector<ModelPrice> prices {
	    {"openai", "gpt-5.4", "completion", 2.50, 15.00, "https://developers.openai.com/api/docs/pricing",
	     "standard text token pricing", "2026-06-30"},
	    {"openai", "gpt-5.4-mini", "completion", 0.75, 4.50,
	     "https://developers.openai.com/api/docs/models/gpt-5.4-mini", "standard text token pricing", "2026-06-30"},
	    {"openai", "gpt-5.4-nano", "completion", 0.20, 1.25, "https://developers.openai.com/api/docs/pricing",
	     "standard text token pricing", "2026-06-30"},
	    {"openai", "text-embedding-3-small", "embedding", 0.02, -1,
	     "https://developers.openai.com/api/docs/models/text-embedding-3-small", "embedding input tokens only",
	     "2026-06-30"},
	    {"claude", "claude-haiku-4-5", "completion", 1.00, 5.00,
	     "https://platform.claude.com/docs/en/about-claude/pricing", "standard text token pricing", "2026-06-30"},
	    {"claude", "claude-3-5-haiku-latest", "completion", 0.80, 4.00,
	     "https://platform.claude.com/docs/en/about-claude/pricing", "legacy Haiku 3.5 pricing", "2026-06-30"},
	    {"claude", "claude-sonnet-4-5", "completion", 3.00, 15.00,
	     "https://platform.claude.com/docs/en/about-claude/pricing", "standard text token pricing", "2026-06-30"},
	    {"gemini", "gemini-2.5-flash", "completion", 0.30, 2.50, "https://ai.google.dev/gemini-api/docs/pricing",
	     "text/image/video input <= 200k tokens", "2026-06-30"},
	    {"gemini", "text-embedding-004", "embedding", 0.00, -1, "https://ai.google.dev/gemini-api/docs/pricing",
	     "listed as free tier pricing", "2026-06-30"},
	    {"mistral", "mistral-small-latest", "completion", 0.10, 0.30, "https://mistral.ai/pricing/",
	     "standard text token pricing", "2026-06-30"},
	    {"mistral", "mistral-embed", "embedding", 0.10, -1, "https://mistral.ai/pricing/",
	     "embedding input tokens only", "2026-06-30"},
	    {"deepseek", "deepseek-chat", "completion", 0.27, 1.10,
	     "https://api-docs.deepseek.com/quick_start/pricing/pricing_details", "input price uses cache-miss pricing",
	     "2026-06-30"},
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

class JsonDocumentValidator {
public:
	explicit JsonDocumentValidator(const std::string &input_p) : input(input_p) {
	}

	bool Validate(std::string &error) {
		SkipWhitespace();
		if (pos == input.size()) {
			error = "empty JSON document";
			return false;
		}
		if (!ParseValue(error)) {
			return false;
		}
		SkipWhitespace();
		if (pos != input.size()) {
			error = "unexpected trailing content at byte " + std::to_string(pos);
			return false;
		}
		return true;
	}

private:
	const std::string &input;
	size_t pos = 0;

	void SkipWhitespace() {
		while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
			pos++;
		}
	}

	bool ConsumeLiteral(const std::string &literal) {
		if (input.compare(pos, literal.size(), literal) != 0) {
			return false;
		}
		pos += literal.size();
		return true;
	}

	bool ParseValue(std::string &error) {
		SkipWhitespace();
		if (pos >= input.size()) {
			error = "expected JSON value";
			return false;
		}
		switch (input[pos]) {
		case '{':
			return ParseObject(error);
		case '[':
			return ParseArray(error);
		case '"':
			return ParseString(error);
		case 't':
			if (ConsumeLiteral("true")) {
				return true;
			}
			break;
		case 'f':
			if (ConsumeLiteral("false")) {
				return true;
			}
			break;
		case 'n':
			if (ConsumeLiteral("null")) {
				return true;
			}
			break;
		default:
			if (input[pos] == '-' || std::isdigit(static_cast<unsigned char>(input[pos]))) {
				return ParseNumber(error);
			}
			break;
		}
		error = "invalid JSON value at byte " + std::to_string(pos);
		return false;
	}

	bool ParseObject(std::string &error) {
		pos++;
		SkipWhitespace();
		if (pos < input.size() && input[pos] == '}') {
			pos++;
			return true;
		}
		while (pos < input.size()) {
			if (pos >= input.size() || input[pos] != '"') {
				error = "expected JSON object key at byte " + std::to_string(pos);
				return false;
			}
			if (!ParseString(error)) {
				return false;
			}
			SkipWhitespace();
			if (pos >= input.size() || input[pos] != ':') {
				error = "expected ':' after JSON object key at byte " + std::to_string(pos);
				return false;
			}
			pos++;
			if (!ParseValue(error)) {
				return false;
			}
			SkipWhitespace();
			if (pos < input.size() && input[pos] == ',') {
				pos++;
				SkipWhitespace();
				continue;
			}
			if (pos < input.size() && input[pos] == '}') {
				pos++;
				return true;
			}
			error = "expected ',' or '}' in JSON object at byte " + std::to_string(pos);
			return false;
		}
		error = "unterminated JSON object";
		return false;
	}

	bool ParseArray(std::string &error) {
		pos++;
		SkipWhitespace();
		if (pos < input.size() && input[pos] == ']') {
			pos++;
			return true;
		}
		while (pos < input.size()) {
			if (!ParseValue(error)) {
				return false;
			}
			SkipWhitespace();
			if (pos < input.size() && input[pos] == ',') {
				pos++;
				SkipWhitespace();
				continue;
			}
			if (pos < input.size() && input[pos] == ']') {
				pos++;
				return true;
			}
			error = "expected ',' or ']' in JSON array at byte " + std::to_string(pos);
			return false;
		}
		error = "unterminated JSON array";
		return false;
	}

	bool ParseString(std::string &error) {
		if (pos >= input.size() || input[pos] != '"') {
			error = "expected JSON string at byte " + std::to_string(pos);
			return false;
		}
		pos++;
		while (pos < input.size()) {
			auto c = input[pos++];
			if (c == '"') {
				return true;
			}
			if (static_cast<unsigned char>(c) < 0x20) {
				error = "unescaped control character in JSON string at byte " + std::to_string(pos - 1);
				return false;
			}
			if (c != '\\') {
				continue;
			}
			if (pos >= input.size()) {
				error = "unterminated JSON escape";
				return false;
			}
			auto esc = input[pos++];
			switch (esc) {
			case '"':
			case '\\':
			case '/':
			case 'b':
			case 'f':
			case 'n':
			case 'r':
			case 't':
				break;
			case 'u':
				if (pos + 4 > input.size()) {
					error = "invalid JSON unicode escape at byte " + std::to_string(pos - 2);
					return false;
				}
				for (size_t i = 0; i < 4; i++) {
					if (!std::isxdigit(static_cast<unsigned char>(input[pos + i]))) {
						error = "invalid JSON unicode escape at byte " + std::to_string(pos - 2);
						return false;
					}
				}
				pos += 4;
				break;
			default:
				error = "invalid JSON escape at byte " + std::to_string(pos - 1);
				return false;
			}
		}
		error = "unterminated JSON string";
		return false;
	}

	bool ParseNumber(std::string &error) {
		auto start = pos;
		if (input[pos] == '-') {
			pos++;
		}
		if (pos >= input.size()) {
			error = "invalid JSON number at byte " + std::to_string(start);
			return false;
		}
		if (input[pos] == '0') {
			pos++;
		} else if (std::isdigit(static_cast<unsigned char>(input[pos]))) {
			while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
				pos++;
			}
		} else {
			error = "invalid JSON number at byte " + std::to_string(start);
			return false;
		}
		if (pos < input.size() && input[pos] == '.') {
			pos++;
			auto fraction_start = pos;
			while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
				pos++;
			}
			if (fraction_start == pos) {
				error = "invalid JSON number fraction at byte " + std::to_string(start);
				return false;
			}
		}
		if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
			pos++;
			if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) {
				pos++;
			}
			auto exponent_start = pos;
			while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
				pos++;
			}
			if (exponent_start == pos) {
				error = "invalid JSON number exponent at byte " + std::to_string(start);
				return false;
			}
		}
		return true;
	}
};

std::string DecodeJsonString(const std::string &input, size_t &pos) {
	if (pos >= input.size() || input[pos] != '"') {
		throw InvalidInputException("Expected JSON string while parsing AI provider response");
	}
	pos++;
	std::string result;
	while (pos < input.size()) {
		auto c = input[pos++];
		if (c == '"') {
			return result;
		}
		if (c != '\\') {
			result.push_back(c);
			continue;
		}
		if (pos >= input.size()) {
			throw InvalidInputException("Invalid JSON escape while parsing AI provider response");
		}
		auto esc = input[pos++];
		switch (esc) {
		case '"':
		case '\\':
		case '/':
			result.push_back(esc);
			break;
		case 'b':
			result.push_back('\b');
			break;
		case 'f':
			result.push_back('\f');
			break;
		case 'n':
			result.push_back('\n');
			break;
		case 'r':
			result.push_back('\r');
			break;
		case 't':
			result.push_back('\t');
			break;
		case 'u':
			if (pos + 4 > input.size()) {
				throw InvalidInputException("Invalid JSON unicode escape while parsing AI provider response");
			}
			// Keep non-ASCII escapes escaped for now. This avoids lossy transcoding in
			// the minimal parser while preserving the response text deterministically.
			result += "\\u" + input.substr(pos, 4);
			pos += 4;
			break;
		default:
			result.push_back(esc);
			break;
		}
	}
	throw InvalidInputException("Unterminated JSON string while parsing AI provider response");
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

class JsonValueParser {
public:
	explicit JsonValueParser(const std::string &input_p) : input(input_p) {
	}

	bool Parse(JsonValue &value, std::string &error) {
		SkipWhitespace();
		if (pos == input.size()) {
			error = "empty JSON document";
			return false;
		}
		if (!ParseValue(value, error)) {
			return false;
		}
		SkipWhitespace();
		if (pos != input.size()) {
			error = "unexpected trailing content at byte " + std::to_string(pos);
			return false;
		}
		return true;
	}

private:
	const std::string &input;
	size_t pos = 0;

	void SkipWhitespace() {
		while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
			pos++;
		}
	}

	bool ConsumeLiteral(const std::string &literal) {
		if (input.compare(pos, literal.size(), literal) != 0) {
			return false;
		}
		pos += literal.size();
		return true;
	}

	bool ParseValue(JsonValue &value, std::string &error) {
		SkipWhitespace();
		if (pos >= input.size()) {
			error = "expected JSON value";
			return false;
		}
		switch (input[pos]) {
		case '{':
			return ParseObject(value, error);
		case '[':
			return ParseArray(value, error);
		case '"':
			return ParseString(value, error);
		case 't':
			if (ConsumeLiteral("true")) {
				value.type = JsonValueType::BOOLEAN;
				value.boolean_value = true;
				return true;
			}
			break;
		case 'f':
			if (ConsumeLiteral("false")) {
				value.type = JsonValueType::BOOLEAN;
				value.boolean_value = false;
				return true;
			}
			break;
		case 'n':
			if (ConsumeLiteral("null")) {
				value.type = JsonValueType::NULL_VALUE;
				return true;
			}
			break;
		default:
			if (input[pos] == '-' || std::isdigit(static_cast<unsigned char>(input[pos]))) {
				return ParseNumber(value, error);
			}
			break;
		}
		error = "invalid JSON value at byte " + std::to_string(pos);
		return false;
	}

	bool ParseObject(JsonValue &value, std::string &error) {
		value.type = JsonValueType::OBJECT;
		pos++;
		SkipWhitespace();
		if (pos < input.size() && input[pos] == '}') {
			pos++;
			return true;
		}
		while (pos < input.size()) {
			std::string key;
			if (!ParseStringValue(key, error)) {
				return false;
			}
			SkipWhitespace();
			if (pos >= input.size() || input[pos] != ':') {
				error = "expected ':' after JSON object key at byte " + std::to_string(pos);
				return false;
			}
			pos++;
			JsonValue member;
			if (!ParseValue(member, error)) {
				return false;
			}
			value.object_value[key] = std::move(member);
			SkipWhitespace();
			if (pos < input.size() && input[pos] == ',') {
				pos++;
				SkipWhitespace();
				continue;
			}
			if (pos < input.size() && input[pos] == '}') {
				pos++;
				return true;
			}
			error = "expected ',' or '}' in JSON object at byte " + std::to_string(pos);
			return false;
		}
		error = "unterminated JSON object";
		return false;
	}

	bool ParseArray(JsonValue &value, std::string &error) {
		value.type = JsonValueType::ARRAY;
		pos++;
		SkipWhitespace();
		if (pos < input.size() && input[pos] == ']') {
			pos++;
			return true;
		}
		while (pos < input.size()) {
			JsonValue element;
			if (!ParseValue(element, error)) {
				return false;
			}
			value.array_value.push_back(std::move(element));
			SkipWhitespace();
			if (pos < input.size() && input[pos] == ',') {
				pos++;
				SkipWhitespace();
				continue;
			}
			if (pos < input.size() && input[pos] == ']') {
				pos++;
				return true;
			}
			error = "expected ',' or ']' in JSON array at byte " + std::to_string(pos);
			return false;
		}
		error = "unterminated JSON array";
		return false;
	}

	bool ParseString(JsonValue &value, std::string &error) {
		value.type = JsonValueType::STRING;
		return ParseStringValue(value.string_value, error);
	}

	bool ParseStringValue(std::string &value, std::string &error) {
		try {
			value = DecodeJsonString(input, pos);
			return true;
		} catch (std::exception &ex) {
			error = ex.what();
			return false;
		}
	}

	bool ParseNumber(JsonValue &value, std::string &error) {
		auto start = pos;
		if (input[pos] == '-') {
			pos++;
		}
		if (pos >= input.size()) {
			error = "invalid JSON number at byte " + std::to_string(start);
			return false;
		}
		if (input[pos] == '0') {
			pos++;
		} else if (std::isdigit(static_cast<unsigned char>(input[pos]))) {
			while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
				pos++;
			}
		} else {
			error = "invalid JSON number at byte " + std::to_string(start);
			return false;
		}
		auto is_integer = true;
		if (pos < input.size() && input[pos] == '.') {
			is_integer = false;
			pos++;
			auto fraction_start = pos;
			while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
				pos++;
			}
			if (fraction_start == pos) {
				error = "invalid JSON number fraction at byte " + std::to_string(start);
				return false;
			}
		}
		if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
			is_integer = false;
			pos++;
			if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) {
				pos++;
			}
			auto exponent_start = pos;
			while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
				pos++;
			}
			if (exponent_start == pos) {
				error = "invalid JSON number exponent at byte " + std::to_string(start);
				return false;
			}
		}
		value.type = JsonValueType::NUMBER;
		value.number_is_integer = is_integer;
		try {
			value.number_value = std::stod(input.substr(start, pos - start));
		} catch (...) {
			error = "invalid JSON number at byte " + std::to_string(start);
			return false;
		}
		return true;
	}
};

bool ParseJsonValueDocument(const std::string &input, JsonValue &value, std::string &error) {
	JsonValueParser parser(input);
	return parser.Parse(value, error);
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
	try {
		std::regex regex(pattern);
		return std::regex_search(value, regex);
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
	auto needle = "\"" + key + "\"";
	size_t pos = 0;
	while ((pos = body.find(needle, pos)) != std::string::npos) {
		pos += needle.size();
		while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) {
			pos++;
		}
		if (pos >= body.size() || body[pos] != ':') {
			continue;
		}
		pos++;
		while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) {
			pos++;
		}
		if (pos < body.size() && body[pos] == '"') {
			value = DecodeJsonString(body, pos);
			return true;
		}
	}
	return false;
}

int64_t FindJsonIntegerValue(const std::string &body, const std::string &key) {
	auto needle = "\"" + key + "\"";
	auto pos = body.find(needle);
	if (pos == std::string::npos) {
		return -1;
	}
	pos += needle.size();
	while (pos < body.size() && (std::isspace(static_cast<unsigned char>(body[pos])) || body[pos] == ':')) {
		pos++;
	}
	auto start = pos;
	while (pos < body.size() && std::isdigit(static_cast<unsigned char>(body[pos]))) {
		pos++;
	}
	if (start == pos) {
		return -1;
	}
	return std::stoll(body.substr(start, pos - start));
}

size_t WriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
	auto response = reinterpret_cast<std::string *>(userdata);
	response->append(ptr, size * nmemb);
	return size * nmemb;
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
		if (max_concurrent_requests < 0 || max_concurrent_requests > 1024) {
			throw InvalidInputException("DUCKDB_AI_MAX_CONCURRENT_REQUESTS must be between 0 and 1024");
		}
		return max_concurrent_requests;
	} catch (InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("DUCKDB_AI_MAX_CONCURRENT_REQUESTS must be an integer between 0 and 1024");
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

class ProviderRequestGuard {
public:
	explicit ProviderRequestGuard(const CompletionOptions &options)
	    : max_concurrent_requests(MaxConcurrentRequests(options)),
	      min_request_interval_ms(MinRequestIntervalMs(options)) {
		if (max_concurrent_requests <= 0 && min_request_interval_ms <= 0) {
			return;
		}

		std::unique_lock<std::mutex> lock(provider_control_mutex);
		while (true) {
			auto concurrency_ready = max_concurrent_requests <= 0 || active_provider_requests < max_concurrent_requests;
			auto now = std::chrono::steady_clock::now();
			auto rate_ready = min_request_interval_ms <= 0 || !has_last_provider_request_start ||
			                  now >= last_provider_request_start + std::chrono::milliseconds(min_request_interval_ms);
			if (concurrency_ready && rate_ready) {
				break;
			}
			if (!concurrency_ready) {
				provider_control_cv.wait(lock);
			} else {
				provider_control_cv.wait_until(lock, last_provider_request_start +
				                                         std::chrono::milliseconds(min_request_interval_ms));
			}
		}
		active_provider_requests++;
		if (min_request_interval_ms > 0) {
			last_provider_request_start = std::chrono::steady_clock::now();
			has_last_provider_request_start = true;
		}
		acquired = true;
	}

	~ProviderRequestGuard() {
		if (!acquired) {
			return;
		}
		std::lock_guard<std::mutex> lock(provider_control_mutex);
		active_provider_requests--;
		provider_control_cv.notify_all();
	}

private:
	int64_t max_concurrent_requests;
	int64_t min_request_interval_ms;
	bool acquired = false;
};

bool IsRetryableHttpStatus(long status) {
	return status == 429 || status >= 500;
}

void SleepBeforeRetry(long retry_backoff_ms) {
	if (retry_backoff_ms <= 0) {
		return;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(retry_backoff_ms));
}

HttpResponse HttpPost(const std::string &url, const std::string &payload, const std::vector<std::string> &headers,
                      long timeout_seconds, bool throw_http_errors = true, long retry_count = 0,
                      long retry_backoff_ms = 1000) {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	int64_t total_elapsed_ms = 0;
	for (long attempt = 0; attempt <= retry_count; attempt++) {
		auto curl = curl_easy_init();
		if (!curl) {
			throw IOException("Could not initialize libcurl for AI request");
		}

		std::string response_body;
		struct curl_slist *header_list = nullptr;
		for (auto &header : headers) {
			header_list = curl_slist_append(header_list, header.c_str());
		}

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

		auto start = std::chrono::steady_clock::now();
		auto result = curl_easy_perform(curl);
		auto end = std::chrono::steady_clock::now();
		long status = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

		curl_slist_free_all(header_list);
		curl_easy_cleanup(curl);

		total_elapsed_ms += std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
		if (attempt < retry_count && (result != CURLE_OK || IsRetryableHttpStatus(status))) {
			SleepBeforeRetry(retry_backoff_ms);
			continue;
		}
		if (result != CURLE_OK) {
			throw IOException("AI provider request failed: %s", curl_easy_strerror(result));
		}
		if (throw_http_errors && (status < 200 || status >= 300)) {
			throw IOException("AI provider returned HTTP %ld: %s", status, response_body);
		}
		return {response_body, status, total_elapsed_ms};
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
	if (provider == "openai_compatible") {
		return {provider, "openai_chat", "gpt-4o-mini", "", "", "OPENAI_COMPATIBLE_API_KEY", "", false};
	}
	if (provider == "azure") {
		return {provider, "openai_chat", "gpt-4o", "", "", "AZURE_OPENAI_API_KEY", "", true};
	}
	if (provider == "deepseek") {
		return {provider, "openai_chat", "deepseek-chat", "", "https://api.deepseek.com", "DEEPSEEK_API_KEY", "", true};
	}
	if (provider == "mistral") {
		return {provider, "openai_chat", "mistral-small-latest", "", "https://api.mistral.ai/v1", "MISTRAL_API_KEY",
		        "",       true};
	}
	if (provider == "zai" || provider == "zhipu") {
		return {"zai", "openai_chat", "glm-4-flash", "", "https://open.bigmodel.cn/api/paas/v4", "ZAI_API_KEY",
		        "",    true};
	}
	if (provider == "gemini") {
		return {provider,
		        "openai_chat",
		        "gemini-2.5-flash",
		        "",
		        "https://generativelanguage.googleapis.com/v1beta/openai",
		        "GEMINI_API_KEY",
		        "",
		        true};
	}
	if (provider == "claude" || provider == "anthropic") {
		return {"claude",
		        "anthropic_messages",
		        "claude-3-5-haiku-latest",
		        "",
		        "https://api.anthropic.com/v1",
		        "ANTHROPIC_API_KEY",
		        "",
		        true};
	}
	if (provider == "ollama") {
		return {provider, "ollama_chat", "llama3.2", "", "http://localhost:11434", "OLLAMA_API_KEY", "", false};
	}

	throw InvalidInputException("Unsupported AI provider \"%s\". Supported providers: ollama, openai, azure, claude, "
	                            "anthropic, gemini, gcp, mistral, zai, deepseek, openrouter, openai_compatible, local",
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
		return config.provider == "azure" ? AzureOpenAIBaseUrl(provider_base_url)
		                                  : TrimTrailingSlash(provider_base_url);
	}
	if (!generic_base_url.empty()) {
		return config.provider == "azure" ? AzureOpenAIBaseUrl(generic_base_url) : TrimTrailingSlash(generic_base_url);
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
	if (config.provider == "claude") {
		auto claude_key = GetEnv("CLAUDE_API_KEY");
		if (!claude_key.empty()) {
			return claude_key;
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
	return config.default_model;
}

ProviderConfig ResolveProviderConfig(const CompletionOptions &options, bool require_api_key) {
	auto config = ProviderDefaults(options.provider);
	config.base_url = options.base_url.empty() ? ResolveBaseUrl(config)
	                                           : (config.provider == "azure" ? AzureOpenAIBaseUrl(options.base_url)
	                                                                         : TrimTrailingSlash(options.base_url));
	config.api_key = options.api_key.empty() ? ResolveApiKey(config) : options.api_key;
	config.model = ResolveModel(config, options.model);
	if (require_api_key && config.requires_api_key && config.api_key.empty()) {
		throw InvalidInputException("AI provider \"%s\" requires an API key. Set %s or DUCKDB_AI_API_KEY.",
		                            config.provider, config.api_key_env);
	}
	return config;
}

std::string RequestEndpoint(const ProviderConfig &config) {
	if (config.protocol == "ollama_chat") {
		return config.base_url + "/api/chat";
	}
	if (config.protocol == "anthropic_messages") {
		return config.base_url + "/messages";
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
	JsonDocumentValidator validator(input);
	return validator.Validate(error);
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
	if (config.protocol == "anthropic_messages") {
		ValidateResponseSchema(options);
		auto format = NormalizeResponseFormat(options);
		if (format == "json_schema" && options.response_schema.empty()) {
			throw InvalidInputException(
			    "AI option \"response_schema\" must be provided when response_format is json_schema");
		}
		auto max_tokens = options.has_max_tokens ? options.max_tokens : 1024;
		auto payload = "{\"model\":\"" + escaped_model + "\",\"max_tokens\":" + std::to_string(max_tokens);
		if (!options.system_prompt.empty()) {
			payload += ",\"system\":\"" + JsonEscape(options.system_prompt) + "\"";
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
	if (provider == "mistral") {
		return "mistral-embed";
	}
	if (provider == "gemini") {
		return "text-embedding-004";
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
	config.base_url = options.base_url.empty() ? ResolveBaseUrl(config)
	                                           : (config.provider == "azure" ? AzureOpenAIBaseUrl(options.base_url)
	                                                                         : TrimTrailingSlash(options.base_url));
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

void SkipWhitespace(const std::string &body, size_t &pos) {
	while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) {
		pos++;
	}
}

bool IsJsonNumberStart(char c) {
	return c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c));
}

std::vector<double> ParseJsonNumberArray(const std::string &body, size_t pos) {
	if (pos >= body.size() || body[pos] != '[') {
		throw InvalidInputException("Expected JSON number array while parsing AI embedding response");
	}
	pos++;
	std::vector<double> values;
	while (pos < body.size()) {
		SkipWhitespace(body, pos);
		if (pos < body.size() && body[pos] == ']') {
			return values;
		}
		if (pos >= body.size() || !IsJsonNumberStart(body[pos])) {
			throw InvalidInputException("Expected JSON number while parsing AI embedding response");
		}
		char *end_ptr = nullptr;
		auto value = std::strtod(body.c_str() + pos, &end_ptr);
		if (end_ptr == body.c_str() + pos) {
			throw InvalidInputException("Invalid JSON number while parsing AI embedding response");
		}
		values.push_back(value);
		pos = static_cast<size_t>(end_ptr - body.c_str());
		SkipWhitespace(body, pos);
		if (pos < body.size() && body[pos] == ',') {
			pos++;
			continue;
		}
		if (pos < body.size() && body[pos] == ']') {
			return values;
		}
		throw InvalidInputException("Expected comma or closing bracket while parsing AI embedding response");
	}
	throw InvalidInputException("Unterminated JSON number array while parsing AI embedding response");
}

std::vector<double> FindFirstJsonNumberArray(const std::string &body, size_t pos) {
	while (pos < body.size()) {
		if (body[pos] != '[') {
			pos++;
			continue;
		}
		auto candidate = pos + 1;
		SkipWhitespace(body, candidate);
		if (candidate < body.size() && body[candidate] == '[') {
			pos = candidate;
			continue;
		}
		if (candidate < body.size() && (body[candidate] == ']' || IsJsonNumberStart(body[candidate]))) {
			return ParseJsonNumberArray(body, pos);
		}
		pos++;
	}
	throw IOException("AI provider embedding response did not contain a numeric embedding array: %s", body);
}

std::vector<double> FindEmbeddingArray(const std::string &body, const std::string &key) {
	auto needle = "\"" + key + "\"";
	auto pos = body.find(needle);
	if (pos == std::string::npos) {
		return {};
	}
	pos = body.find('[', pos + needle.size());
	if (pos == std::string::npos) {
		return {};
	}
	return FindFirstJsonNumberArray(body, pos);
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
	result.prompt_tokens = FindJsonIntegerValue(response.body, "prompt_tokens");
	result.total_tokens = FindJsonIntegerValue(response.body, "total_tokens");
	return result;
}

std::vector<std::string> RequestHeaders(const ProviderConfig &config) {
	std::vector<std::string> headers {"Content-Type: application/json"};
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
			headers.push_back("X-Title: " + title);
		}
	}
	return headers;
}

std::string ExtractCompletionText(const ProviderConfig &config, const std::string &body) {
	std::string value;
	if (config.protocol == "anthropic_messages") {
		if (FindJsonStringValue(body, "text", value)) {
			return value;
		}
	} else if (FindJsonStringValue(body, "content", value)) {
		return value;
	}
	throw IOException("AI provider response did not contain a supported completion text field: %s", body);
}

CompletionResult ParseCompletionResult(const ProviderConfig &config, const HttpResponse &response) {
	CompletionResult result;
	result.text = ExtractCompletionText(config, response.body);
	result.raw_response = response.body;
	result.http_status = response.status;
	result.elapsed_ms = response.elapsed_ms;
	result.prompt_tokens = FindJsonIntegerValue(response.body, "prompt_tokens");
	result.completion_tokens = FindJsonIntegerValue(response.body, "completion_tokens");
	result.total_tokens = FindJsonIntegerValue(response.body, "total_tokens");

	if (config.protocol == "anthropic_messages") {
		result.prompt_tokens = FindJsonIntegerValue(response.body, "input_tokens");
		result.completion_tokens = FindJsonIntegerValue(response.body, "output_tokens");
		if (result.prompt_tokens >= 0 && result.completion_tokens >= 0) {
			result.total_tokens = result.prompt_tokens + result.completion_tokens;
		}
	}
	if (config.protocol == "ollama_chat") {
		result.prompt_tokens = FindJsonIntegerValue(response.body, "prompt_eval_count");
		result.completion_tokens = FindJsonIntegerValue(response.body, "eval_count");
		if (result.prompt_tokens >= 0 && result.completion_tokens >= 0) {
			result.total_tokens = result.prompt_tokens + result.completion_tokens;
		}
	}
	return result;
}

double EstimateCompletionCostUsd(const ProviderConfig &config, const CompletionOptions &options,
                                 const CompletionResult &result) {
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
	auto cost = (static_cast<double>(result.prompt_tokens) * input_price +
	             static_cast<double>(result.completion_tokens) * output_price) /
	            1000000.0;
	return HasEstimatedCost(cost) ? cost : -1;
}

double EstimateEmbeddingCostUsd(const ProviderConfig &config, const CompletionOptions &options,
                                const EmbeddingResult &result) {
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

void RecordUsageEvent(const ProviderConfig &config, const std::string &prompt, const CompletionResult &result,
                      const CompletionOptions &options) {
	UsageEvent event;
	event.created_at = CurrentTimestamp();
	event.event = "ai_completion";
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
	event.elapsed_ms = result.elapsed_ms;
	event.http_status = result.http_status;
	event.estimated_cost_usd = EstimateCompletionCostUsd(config, options, result);

	std::lock_guard<std::mutex> lock(usage_mutex);
	event.event_id = next_usage_event_id++;
	usage_events.push_back(std::move(event));
	if (usage_events.size() > MAX_USAGE_EVENTS) {
		usage_events.erase(usage_events.begin(), usage_events.begin() + (usage_events.size() - MAX_USAGE_EVENTS));
	}
}

void RecordEmbeddingUsageEvent(const ProviderConfig &config, const std::string &input, const EmbeddingResult &result,
                               const CompletionOptions &options) {
	UsageEvent event;
	event.created_at = CurrentTimestamp();
	event.event = "ai_embedding";
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
	event.elapsed_ms = result.elapsed_ms;
	event.http_status = result.http_status;
	event.estimated_cost_usd = EstimateEmbeddingCostUsd(config, options, result);

	std::lock_guard<std::mutex> lock(usage_mutex);
	event.event_id = next_usage_event_id++;
	usage_events.push_back(std::move(event));
	if (usage_events.size() > MAX_USAGE_EVENTS) {
		usage_events.erase(usage_events.begin(), usage_events.begin() + (usage_events.size() - MAX_USAGE_EVENTS));
	}
}

void PushUsageEvent(UsageEvent event) {
	std::lock_guard<std::mutex> lock(usage_mutex);
	event.event_id = next_usage_event_id++;
	usage_events.push_back(std::move(event));
	if (usage_events.size() > MAX_USAGE_EVENTS) {
		usage_events.erase(usage_events.begin(), usage_events.begin() + (usage_events.size() - MAX_USAGE_EVENTS));
	}
}

bool EnvFlagEnabled(const std::string &name) {
	auto value = LowerAscii(GetEnv(name));
	return value == "1" || value == "true" || value == "yes" || value == "on";
}

double LogSampleRate(const CompletionOptions &options) {
	if (options.has_log_sample_rate) {
		return options.log_sample_rate;
	}
	auto configured = GetEnv("DUCKDB_AI_LOG_SAMPLE_RATE");
	if (configured.empty()) {
		return 1.0;
	}
	try {
		auto rate = std::stod(configured);
		if (rate < 0 || rate > 1) {
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
	throw InvalidInputException("DUCKDB_AI_LOG_FORMAT must be one of: generic_json, otlp_json");
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

std::string OtlpLogPayload(const std::string &event, const std::string &provider, const std::string &protocol,
                           const std::string &model, const std::string &log_tags, const std::string &text_name,
                           const std::string &text_value, const std::string &extra_text_name,
                           const std::string &extra_text_value, bool include_text,
                           const std::vector<std::pair<std::string, int64_t>> &metrics, double estimated_cost_usd) {
	std::string attributes;
	bool wrote_attribute = false;
	AppendJsonAttribute(attributes, wrote_attribute, OtlpStringAttribute("ai.event", event));
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
		    {"ai.elapsed_ms", result.elapsed_ms},
		    {"http.status_code", result.http_status},
		};
		payload = OtlpLogPayload("ai_completion", config.provider, config.protocol, config.model, log_tags, "ai.prompt",
		                         prompt, "ai.response", result.text, include_text, metrics, estimated_cost_usd);
	} else {
		payload = std::string("{") + "\"extension\":\"duckdb_ai\"," + "\"event\":\"ai_completion\"," +
		          "\"provider\":\"" + JsonEscape(config.provider) + "\"," + "\"protocol\":\"" +
		          JsonEscape(config.protocol) + "\"," + "\"model\":\"" + JsonEscape(config.model) + "\"," +
		          "\"prompt_chars\":" + std::to_string(prompt.size()) + "," +
		          "\"response_chars\":" + std::to_string(result.text.size()) + "," +
		          "\"prompt_tokens\":" + std::to_string(result.prompt_tokens) + "," +
		          "\"completion_tokens\":" + std::to_string(result.completion_tokens) + "," +
		          "\"total_tokens\":" + std::to_string(result.total_tokens) + "," +
		          "\"elapsed_ms\":" + std::to_string(result.elapsed_ms) + "," +
		          "\"http_status\":" + std::to_string(result.http_status);

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

	try {
		HttpPost(log_endpoint, payload, {"Content-Type: application/json"}, TimeoutSeconds(options), true,
		         RetryCount(options), RetryBackoffMs(options));
	} catch (std::exception &ex) {
		auto log_strict = options.has_log_strict ? options.log_strict : EnvFlagEnabled("DUCKDB_AI_LOG_STRICT");
		if (log_strict) {
			throw IOException("AI usage log request failed: %s", ex.what());
		}
	}
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
		    {"http.status_code", result.http_status},
		};
		payload = OtlpLogPayload("ai_embedding", config.provider, config.protocol, config.model, log_tags, "ai.input",
		                         input, "", "", include_text, metrics, estimated_cost_usd);
	} else {
		payload = std::string("{") + "\"extension\":\"duckdb_ai\"," + "\"event\":\"ai_embedding\"," +
		          "\"provider\":\"" + JsonEscape(config.provider) + "\"," + "\"protocol\":\"" +
		          JsonEscape(config.protocol) + "\"," + "\"model\":\"" + JsonEscape(config.model) + "\"," +
		          "\"input_chars\":" + std::to_string(input.size()) + "," +
		          "\"dimensions\":" + std::to_string(result.values.size()) + "," +
		          "\"prompt_tokens\":" + std::to_string(result.prompt_tokens) + "," +
		          "\"total_tokens\":" + std::to_string(result.total_tokens) + "," +
		          "\"elapsed_ms\":" + std::to_string(result.elapsed_ms) + "," +
		          "\"http_status\":" + std::to_string(result.http_status);

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

	try {
		HttpPost(log_endpoint, payload, {"Content-Type: application/json"}, TimeoutSeconds(options), true,
		         RetryCount(options), RetryBackoffMs(options));
	} catch (std::exception &ex) {
		auto log_strict = options.has_log_strict ? options.log_strict : EnvFlagEnabled("DUCKDB_AI_LOG_STRICT");
		if (log_strict) {
			throw IOException("AI embedding log request failed: %s", ex.what());
		}
	}
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

std::string ProviderBaseUrl(const std::string &provider) {
	return ResolveBaseUrl(ProviderDefaults(provider));
}

std::string ProviderProtocol(const std::string &provider) {
	return ProviderDefaults(provider).protocol;
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
	auto response = [&]() {
		ProviderRequestGuard request_guard(options);
		return HttpPost(RequestEndpoint(config), RequestPayload(config, prompt, options), RequestHeaders(config),
		                TimeoutSeconds(options), false, RetryCount(options), RetryBackoffMs(options));
	}();
	ThrowIfProviderHttpError(config, response);
	auto result = ParseCompletionResult(config, response);
	RecordUsageEvent(config, prompt, result, options);
	MaybePostUsageLog(config, prompt, result, options);
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
	auto response = [&]() {
		ProviderRequestGuard request_guard(options);
		return HttpPost(EmbeddingEndpoint(config), EmbeddingPayload(config, input), RequestHeaders(config),
		                TimeoutSeconds(options), false, RetryCount(options), RetryBackoffMs(options));
	}();
	ThrowIfProviderHttpError(config, response);
	auto result = ParseEmbeddingResult(config, response);
	RecordEmbeddingUsageEvent(config, input, result, options);
	MaybePostEmbeddingLog(config, input, result, options);
	return result;
}

std::vector<UsageEvent> UsageEvents() {
	std::lock_guard<std::mutex> lock(usage_mutex);
	return usage_events;
}

void ClearUsageEvents() {
	std::lock_guard<std::mutex> lock(usage_mutex);
	usage_events.clear();
}

std::vector<ModelPrice> ModelPrices() {
	return BuiltinModelPrices();
}

void RecordLocalUsageEvent(const std::string &event_name, int64_t input_chars, int64_t response_chars) {
	UsageEvent event;
	event.created_at = CurrentTimestamp();
	event.event = event_name;
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
	event.elapsed_ms = 0;
	event.http_status = 0;
	event.estimated_cost_usd = -1;
	PushUsageEvent(std::move(event));
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
