#pragma once

#include <string>

namespace duckdb {
namespace gcal {

class IAuthProvider {
public:
	virtual ~IAuthProvider() = default;
	virtual std::string GetAuthorizationHeader() = 0;
};

} // namespace gcal
} // namespace duckdb
