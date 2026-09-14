#pragma once

#include <string>

#include "json.hpp"

#include "calendar/resources/base.hpp"
#include "calendar/types.hpp"

namespace duckdb {
namespace gcal {

class CalendarsResource : protected BaseResource {
public:
	CalendarsResource(IHttpClient &http, const HttpHeaders &headers, const std::string &baseUrl)
	    : BaseResource(http, headers, baseUrl) {};

	// Creates a secondary calendar (POST /calendars). `body` carries the writable fields (summary is
	// required by the API). Returns the created calendar, incl. its generated id.
	CalendarListEntry Insert(const nlohmann::json &body);

	// Permanently deletes a secondary calendar (DELETE /calendars/{id}).
	void Delete(const std::string &calendarId);
};

} // namespace gcal
} // namespace duckdb
