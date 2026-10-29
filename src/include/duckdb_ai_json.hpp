#pragma once

#include <string>

namespace duckdb {
namespace duckdb_ai {
namespace detail {

//! Escape JSON string contents without adding quotes or changing UTF-8 bytes.
//! SQL values historically use Unicode escapes for backspace/form feed, while
//! provider payloads use short escapes. Preserve their bytes for cache keys and limits.
inline std::string EscapeJsonString(const std::string &input, bool short_backspace_formfeed) {
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
			out += short_backspace_formfeed ? "\\b" : "\\u0008";
			break;
		case '\f':
			out += short_backspace_formfeed ? "\\f" : "\\u000c";
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

} // namespace detail
} // namespace duckdb_ai
} // namespace duckdb
