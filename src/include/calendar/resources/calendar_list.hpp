#pragma once

#include <string>

#include "calendar/resources/base.hpp"
#include "calendar/types.hpp"

namespace duckdb {
namespace gcal {

class CalendarListResource : protected BaseResource {
public:
	CalendarListResource(IHttpClient &http, const HttpHeaders &headers, const std::string &baseUrl)
	    : BaseResource(http, headers, baseUrl) {};

	CalendarListResponse List(const std::string &pageToken = "");
};

} // namespace gcal
} // namespace duckdb
