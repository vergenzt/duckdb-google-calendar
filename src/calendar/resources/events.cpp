#include "json.hpp"

#include "calendar/resources/events.hpp"
#include "calendar/util/response.hpp"
#include "calendar/util/query.hpp"
#include "calendar/exception.hpp"

using json = nlohmann::json;

namespace duckdb {
namespace gcal {

json EventsResource::List(const std::string &queryString) {
	std::string path = "/calendars/" + UrlEncode(calendarId) + "/events" + queryString;
	return ParseResponse<json>(DoGet(path));
}

json EventsResource::Get(const std::string &eventId) {
	std::string path = "/calendars/" + UrlEncode(calendarId) + "/events/" + UrlEncode(eventId);
	return ParseResponse<json>(DoGet(path));
}

json EventsResource::Insert(const json &event) {
	std::string path = "/calendars/" + UrlEncode(calendarId) + "/events";
	return ParseResponse<json>(DoPost(path, event.dump()));
}

json EventsResource::Update(const std::string &eventId, const json &event) {
	std::string path = "/calendars/" + UrlEncode(calendarId) + "/events/" + UrlEncode(eventId);
	return ParseResponse<json>(DoPut(path, event.dump()));
}

void EventsResource::Delete(const std::string &eventId) {
	std::string path = "/calendars/" + UrlEncode(calendarId) + "/events/" + UrlEncode(eventId);
	auto response = DoDelete(path);
	// events.delete returns 204 No Content on success (200 also accepted defensively).
	if (response.statusCode != 200 && response.statusCode != 204) {
		throw CalendarApiException(response.statusCode, response.body);
	}
}

} // namespace gcal
} // namespace duckdb
