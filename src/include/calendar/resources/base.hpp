#pragma once
#include <string>

#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

class BaseResource {
protected:
	BaseResource(IHttpClient &http, const HttpHeaders &headers, const std::string &baseUrl)
	    : http(http), headers(headers), baseUrl(baseUrl) {};

	IHttpClient &http;
	const HttpHeaders &headers;
	std::string baseUrl;

	HttpResponse DoGet(const std::string &path);
	HttpResponse DoPost(const std::string &path, const std::string &body);
	HttpResponse DoPut(const std::string &path, const std::string &body);
	HttpResponse DoDelete(const std::string &path);
};

} // namespace gcal
} // namespace duckdb
