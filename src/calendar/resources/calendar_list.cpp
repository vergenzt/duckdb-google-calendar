#include "calendar/resources/calendar_list.hpp"
#include "calendar/util/response.hpp"
#include "calendar/util/query.hpp"
#include "calendar/types.hpp"

namespace duckdb {
namespace gcal {

CalendarListResponse CalendarListResource::List(const std::string &pageToken) {
	std::string path = "/users/me/calendarList";
	if (!pageToken.empty()) {
		path += "?pageToken=" + UrlEncode(pageToken);
	}
	return ParseResponse<CalendarListResponse>(DoGet(path));
}

} // namespace gcal
} // namespace duckdb
