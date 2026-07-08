#pragma once

#include "json.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <unordered_map>

namespace duckdb {
namespace gcal_map {

// Read mapping (JSON -> column) lives in calendar_scan.cpp's ExtractField. The write mapping below
// is keyed by column name, so it stays correct regardless of column order in AddEventsColumns.

inline string FormatRfc3339(const Value &val) {
	auto tstz = val.GetValue<timestamp_tz_t>();
	timestamp_t ts(tstz.value);
	string s = Timestamp::ToString(ts);
	std::replace(s.begin(), s.end(), ' ', 'T');
	return s + "Z";
}

inline string FormatDate(const Value &val) {
	auto tstz = val.GetValue<timestamp_tz_t>();
	timestamp_t ts(tstz.value);
	string s = Timestamp::ToString(ts);
	return s.substr(0, 10);
}

inline nlohmann::json TimeNode(const Value &val, bool all_day) {
	nlohmann::json node;
	if (all_day) {
		node["date"] = FormatDate(val);
	} else {
		node["dateTime"] = FormatRfc3339(val);
	}
	return node;
}

inline void SetJsonPassthrough(nlohmann::json &event, const char *key, const Value &val) {
	if (val.IsNull()) {
		return;
	}
	auto text = val.ToString();
	if (text.empty()) {
		return;
	}
	try {
		event[key] = nlohmann::json::parse(text);
	} catch (...) {
		// best-effort: ignore malformed passthrough JSON
	}
}

inline bool ExistingAllDay(const nlohmann::json &event) {
	return event.contains("start") && event["start"].is_object() && event["start"].contains("date") &&
	       !event["start"].contains("dateTime");
}

// Re-encode an existing start/end node when only all_day flips.
inline void ReencodeTimeNode(nlohmann::json &event, const char *key, bool all_day) {
	if (!event.contains(key) || !event[key].is_object()) {
		return;
	}
	auto &node = event[key];
	string raw;
	if (node.contains("dateTime") && node["dateTime"].is_string()) {
		raw = node["dateTime"].get<string>();
	} else if (node.contains("date") && node["date"].is_string()) {
		raw = node["date"].get<string>();
	} else {
		return;
	}
	nlohmann::json rebuilt;
	if (all_day) {
		rebuilt["date"] = raw.substr(0, 10);
	} else {
		rebuilt["dateTime"] = raw.size() == 10 ? (raw + "T00:00:00Z") : raw;
	}
	event[key] = rebuilt;
}

// Writable columns -> Google JSON key, grouped by encoding. Anything not listed here is
// server-managed / read-only (id is the UPDATE key, so it is settable only on INSERT).
inline const std::unordered_map<string, const char *> &WritableStringKeys() {
	static const std::unordered_map<string, const char *> m = {
	    {"summary", "summary"},   {"description", "description"},   {"location", "location"},
	    {"status", "status"},     {"color_id", "colorId"},          {"transparency", "transparency"},
	    {"visibility", "visibility"}, {"event_type", "eventType"},
	};
	return m;
}
inline const std::unordered_map<string, const char *> &WritableBoolKeys() {
	static const std::unordered_map<string, const char *> m = {
	    {"anyone_can_add_self", "anyoneCanAddSelf"},
	    {"guests_can_invite_others", "guestsCanInviteOthers"},
	    {"guests_can_modify", "guestsCanModify"},
	    {"guests_can_see_other_guests", "guestsCanSeeOtherGuests"},
	};
	return m;
}
inline const std::unordered_map<string, const char *> &WritableJsonKeys() {
	static const std::unordered_map<string, const char *> m = {
	    {"attendees", "attendees"},
	    {"recurrence", "recurrence"},
	    {"reminders", "reminders"},
	    {"conference_data", "conferenceData"},
	    {"extended_properties", "extendedProperties"},
	    {"source", "source"},
	    {"attachments", "attachments"},
	    {"working_location_properties", "workingLocationProperties"},
	};
	return m;
}

// Apply one column's value to `event`, keyed by column name. update=true gives PATCH semantics
// (NULL erases the key); update=false (INSERT) simply omits NULLs. Returns false if the column is
// not writable in this mode (read-only / server-managed), which UPDATE turns into an error.
inline bool ApplyColumn(nlohmann::json &event, const string &name, const Value &val, bool all_day, bool update) {
	if (name == "id") {
		// client-supplied id (base32hex, len 5-1024) on INSERT; the PATCH key (not settable) on UPDATE.
		if (update) {
			return false;
		}
		if (!val.IsNull()) {
			event["id"] = val.ToString();
		}
		return true;
	}
	auto s = WritableStringKeys().find(name);
	if (s != WritableStringKeys().end()) {
		if (val.IsNull()) {
			if (update) {
				event.erase(s->second);
			}
		} else {
			event[s->second] = val.ToString();
		}
		return true;
	}
	auto b = WritableBoolKeys().find(name);
	if (b != WritableBoolKeys().end()) {
		if (val.IsNull()) {
			if (update) {
				event.erase(b->second);
			}
		} else {
			event[b->second] = BooleanValue::Get(val);
		}
		return true;
	}
	auto j = WritableJsonKeys().find(name);
	if (j != WritableJsonKeys().end()) {
		if (val.IsNull()) {
			if (update) {
				event.erase(j->second);
			}
		} else {
			SetJsonPassthrough(event, j->second, val);
		}
		return true;
	}
	if (name == "start" || name == "end") {
		const char *key = name.c_str();
		if (val.IsNull()) {
			if (update) {
				event.erase(key);
			}
		} else {
			event[key] = TimeNode(val, all_day);
		}
		return true;
	}
	if (name == "all_day") {
		// value carried via the all_day flag; on UPDATE re-encode existing nodes not also SET here.
		if (update) {
			ReencodeTimeNode(event, "start", all_day);
			ReencodeTimeNode(event, "end", all_day);
		}
		return true;
	}
	return false;
}

// Does this row/SET carry an all_day=true value? (Drives start/end date-vs-dateTime encoding.)
inline bool RowAllDay(DataChunk &chunk, idx_t row, const vector<string> &names) {
	for (idx_t c = 0; c < names.size(); c++) {
		if (names[c] == "all_day") {
			auto v = chunk.GetValue(c, row);
			return !v.IsNull() && BooleanValue::Get(v);
		}
	}
	return false;
}

// Build a full event body from an INSERT row. `names` maps chunk column -> schema column name.
inline nlohmann::json RowToEvent(DataChunk &chunk, idx_t row, const vector<string> &names) {
	nlohmann::json event;
	bool all_day = RowAllDay(chunk, row, names);
	for (idx_t c = 0; c < names.size(); c++) {
		ApplyColumn(event, names[c], chunk.GetValue(c, row), all_day, /*update=*/false);
	}
	return event;
}

// Apply one SET column (by name) to an existing event during UPDATE.
inline void ApplySet(nlohmann::json &event, const string &name, const Value &val, bool all_day) {
	if (!ApplyColumn(event, name, val, all_day, /*update=*/true)) {
		throw InvalidInputException("google_calendar: column \"%s\" is read-only and cannot be updated", name);
	}
}

} // namespace gcal_map
} // namespace duckdb
