#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace duckdb {
namespace gcal {

// Calendar enumeration (attach-time). Event payloads are handled as raw nlohmann::json
// (VARCHAR passthrough, D6) in the scan (Slice 7) and DML (Slice 8), not typed here.
struct CalendarListEntry {
	std::string id = "";
	std::string summary = "";
	std::string description = "";
	std::string timeZone = "";
	bool primary = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CalendarListEntry, id, summary, description, timeZone, primary)

struct CalendarListResponse {
	std::vector<CalendarListEntry> items = {};
	std::string nextPageToken = "";
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CalendarListResponse, items, nextPageToken)

} // namespace gcal
} // namespace duckdb
