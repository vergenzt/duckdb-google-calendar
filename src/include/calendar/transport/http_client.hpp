#pragma once

#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

class IHttpClient {
public:
	virtual ~IHttpClient() = default;
	virtual HttpResponse Execute(const HttpRequest &request) = 0;

	HttpResponse Post(const std::string &url, const HttpHeaders &headers, const std::string &body);
};
} // namespace gcal
} // namespace duckdb
