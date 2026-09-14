#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace duckdb {
namespace gcal {

// One entry of users/me/calendarList. Backs the `calendars` table (D15) and supplies the calendar
// ids the `events` table fans out over. Event payloads stay raw nlohmann::json (VARCHAR
// passthrough, D6) in the scan and DML rather than being typed here.
struct CalendarListEntry {
	std::string id = "";
	std::string summary = "";
	std::string summaryOverride = "";
	std::string description = "";
	std::string location = "";
	std::string timeZone = "";
	std::string colorId = "";
	std::string backgroundColor = "";
	std::string foregroundColor = "";
	std::string accessRole = "";
	std::string etag = "";
	std::string kind = "";
	bool primary = false;
	bool selected = false;
	bool hidden = false;
	bool deleted = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CalendarListEntry, id, summary, summaryOverride, description, location,
                                                timeZone, colorId, backgroundColor, foregroundColor, accessRole, etag,
                                                kind, primary, selected, hidden, deleted)

struct CalendarListResponse {
	std::vector<CalendarListEntry> items = {};
	std::string nextPageToken = "";
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CalendarListResponse, items, nextPageToken)

} // namespace gcal
} // namespace duckdb
