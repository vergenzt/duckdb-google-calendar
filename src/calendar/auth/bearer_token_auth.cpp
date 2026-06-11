#include "calendar/auth/bearer_token_auth.hpp"

namespace duckdb {
namespace gcal {

std::string BearerTokenAuth::GetAuthorizationHeader() {
	return "Bearer " + token;
}

} // namespace gcal
} // namespace duckdb
