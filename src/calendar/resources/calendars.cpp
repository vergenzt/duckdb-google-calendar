#include "json.hpp"

#include "calendar/resources/calendars.hpp"
#include "calendar/util/response.hpp"
#include "calendar/util/query.hpp"
#include "calendar/exception.hpp"

using json = nlohmann::json;

namespace duckdb {
namespace gcal {

CalendarListEntry CalendarsResource::Insert(const std::string &summary) {
	json body = {{"summary", summary}};
	return ParseResponse<CalendarListEntry>(DoPost("/calendars", body.dump()));
}

void CalendarsResource::Delete(const std::string &calendarId) {
	auto response = DoDelete("/calendars/" + UrlEncode(calendarId));
	// calendars.delete returns 204 No Content on success (200 accepted defensively).
	if (response.statusCode != 200 && response.statusCode != 204) {
		throw CalendarApiException(response.statusCode, response.body);
	}
}

} // namespace gcal
} // namespace duckdb
