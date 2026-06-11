#pragma once

#include <string>

#include "calendar/auth/auth_provider.hpp"

namespace duckdb {
namespace gcal {

class BearerTokenAuth : public IAuthProvider {
public:
	explicit BearerTokenAuth(const std::string &token) : token(token) {
	}

	std::string GetAuthorizationHeader() override;

private:
	std::string token;
};

} // namespace gcal
} // namespace duckdb
