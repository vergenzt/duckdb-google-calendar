#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

HttpResponse IHttpClient::Post(const std::string &url, const HttpHeaders &headers, const std::string &body) {
	return Execute(HttpRequest {HttpMethod::POST, url, headers, body});
}
} // namespace gcal
} // namespace duckdb
