#pragma once

#include <string>

#include "calendar/util/version.hpp"

#include "calendar/auth/auth_provider.hpp"
#include "calendar/resources/calendar_list.hpp"
#include "calendar/resources/events.hpp"
#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

constexpr const char *DEFAULT_CALENDAR_API_URL = "https://www.googleapis.com/calendar/v3";

class GoogleCalendarClient {
public:
	GoogleCalendarClient(IHttpClient &http, IAuthProvider &auth,
	                     const std::string &baseUrl = DEFAULT_CALENDAR_API_URL)
	    : http(http), headers(BuildHeaders(auth)), baseUrl(baseUrl) {
	}

	CalendarListResource CalendarList() {
		return CalendarListResource(http, headers, baseUrl);
	}

	EventsResource Events(const std::string &calendarId) {
		return EventsResource(http, headers, baseUrl, calendarId);
	}

private:
	IHttpClient &http;
	HttpHeaders headers;
	std::string baseUrl;

	static HttpHeaders BuildHeaders(IAuthProvider &auth) {
		HttpHeaders h;
		h["Authorization"] = auth.GetAuthorizationHeader();
		h["Content-Type"] = "application/json";
		h["Accept"] = "application/json";

		std::string version = getVersion();
		h["User-Agent"] = "duckdb-google-calendar/" + (version.empty() ? std::string("dev") : version);

		return h;
	}
};
} // namespace gcal
} // namespace duckdb
