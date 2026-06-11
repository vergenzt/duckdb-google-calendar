#include <cctype>

#include "calendar/util/query.hpp"

namespace duckdb {
namespace gcal {

std::string UrlEncode(const std::string &value) {
	static const char hex[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(value.size() * 3);
	for (unsigned char c : value) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			result += static_cast<char>(c);
		} else {
			result += '%';
			result += hex[(c >> 4) & 0xF];
			result += hex[c & 0xF];
		}
	}
	return result;
}

QueryBuilder &QueryBuilder::Add(const std::string &key, const std::string &value) {
	params.emplace_back(key, value);
	return *this;
}

std::string QueryBuilder::Build() const {
	if (params.empty()) {
		return "";
	}
	std::string result = "?";
	bool first = true;
	for (const auto &p : params) {
		if (!first) {
			result += "&";
		}
		first = false;
		result += UrlEncode(p.first) + "=" + UrlEncode(p.second);
	}
	return result;
}

} // namespace gcal
} // namespace duckdb
