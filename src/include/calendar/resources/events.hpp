#pragma once

#include <string>

#include "json.hpp"

#include "calendar/resources/base.hpp"
#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

class EventsResource : protected BaseResource {
public:
	EventsResource(IHttpClient &http, const HttpHeaders &headers, const std::string &baseUrl,
	               const std::string &calendarId)
	    : BaseResource(http, headers, baseUrl), calendarId(calendarId) {};

	// queryString is the already-built "?k=v&..." suffix (or "").
	nlohmann::json List(const std::string &queryString);
	nlohmann::json Get(const std::string &eventId);
	nlohmann::json Insert(const nlohmann::json &event);
	nlohmann::json Update(const std::string &eventId, const nlohmann::json &event);
	void Delete(const std::string &eventId);

private:
	std::string calendarId;
};

} // namespace gcal
} // namespace duckdb
