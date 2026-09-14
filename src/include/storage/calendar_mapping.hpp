#pragma once

#include "calendar/types.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"

#include "json.hpp"

namespace duckdb {
namespace gcal_map {

// Mapping for the `calendars` table. Unlike events (raw JSON passthrough), calendarList entries are
// small and fully typed, so they go through gcal::CalendarListEntry.

// An empty Google string field means "absent"; surface that as SQL NULL rather than ''.
inline Value CalendarString(const std::string &value) {
	return value.empty() ? Value(LogicalType::VARCHAR) : Value(value);
}

// One column of a calendars row, keyed by column name so it is independent of column order.
inline Value ExtractCalendarField(const gcal::CalendarListEntry &cal, const string &field) {
	if (field == "calendar_id") {
		return CalendarString(cal.id);
	}
	if (field == "summary") {
		return CalendarString(cal.summary);
	}
	if (field == "description") {
		return CalendarString(cal.description);
	}
	if (field == "location") {
		return CalendarString(cal.location);
	}
	if (field == "time_zone") {
		return CalendarString(cal.timeZone);
	}
	if (field == "summary_override") {
		return CalendarString(cal.summaryOverride);
	}
	if (field == "access_role") {
		return CalendarString(cal.accessRole);
	}
	if (field == "color_id") {
		return CalendarString(cal.colorId);
	}
	if (field == "background_color") {
		return CalendarString(cal.backgroundColor);
	}
	if (field == "foreground_color") {
		return CalendarString(cal.foregroundColor);
	}
	if (field == "etag") {
		return CalendarString(cal.etag);
	}
	if (field == "kind") {
		return CalendarString(cal.kind);
	}
	if (field == "is_primary") {
		return Value::BOOLEAN(cal.primary);
	}
	if (field == "selected") {
		return Value::BOOLEAN(cal.selected);
	}
	if (field == "hidden") {
		return Value::BOOLEAN(cal.hidden);
	}
	if (field == "deleted") {
		return Value::BOOLEAN(cal.deleted);
	}
	return Value();
}

inline vector<Value> CalendarToRow(const gcal::CalendarListEntry &cal, const vector<string> &names) {
	vector<Value> row;
	row.reserve(names.size());
	for (auto &name : names) {
		row.push_back(ExtractCalendarField(cal, name));
	}
	return row;
}

// Build a calendars.insert body from an INSERT row. Only the four fields calendars.insert accepts are
// carried; everything else on the calendars schema is calendarList overlay or server-managed, so a
// non-NULL value there is a user error rather than something to silently drop.
inline nlohmann::json RowToCalendar(DataChunk &chunk, idx_t row, const vector<string> &names) {
	static const std::unordered_map<string, const char *> writable = {
	    {"summary", "summary"},
	    {"description", "description"},
	    {"location", "location"},
	    {"time_zone", "timeZone"},
	};
	nlohmann::json body;
	for (idx_t c = 0; c < names.size(); c++) {
		auto val = chunk.GetValue(c, row);
		if (val.IsNull()) {
			continue;
		}
		auto it = writable.find(names[c]);
		if (it != writable.end()) {
			body[it->second] = val.ToString();
			continue;
		}
		if (names[c] == "calendar_id") {
			// Google assigns the id; accepting one would silently create a differently-named calendar.
			throw InvalidInputException("google_calendar: calendar_id is assigned by Google and cannot be set on "
			                            "INSERT INTO calendars; omit it and read it back with RETURNING");
		}
		throw InvalidInputException(
		    "google_calendar: column \"%s\" is read-only on INSERT INTO calendars (writable: summary, "
		    "description, location, time_zone)",
		    names[c]);
	}
	if (!body.contains("summary")) {
		throw InvalidInputException("google_calendar: INSERT INTO calendars requires a summary (the calendar's name)");
	}
	return body;
}

} // namespace gcal_map
} // namespace duckdb
