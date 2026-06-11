#pragma once

#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace gcal {

std::string UrlEncode(const std::string &value);

class QueryBuilder {
public:
	QueryBuilder &Add(const std::string &key, const std::string &value);
	// Returns "" when empty, otherwise "?k1=v1&k2=v2" (percent-encoded).
	std::string Build() const;

private:
	std::vector<std::pair<std::string, std::string>> params;
};

} // namespace gcal
} // namespace duckdb
