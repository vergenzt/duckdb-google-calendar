#pragma once

#include "json.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>

namespace duckdb {
namespace gcal_map {

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
	return event.contains(EVENT_COL_START_AT.Name()) && event[EVENT_COL_START_AT.Name()].is_object() && event[EVENT_COL_START_AT.Name()].contains("date") &&
	       !event[EVENT_COL_START_AT.Name()].contains("dateTime");
}

// Build a full event body from an INSERT row (all 15 columns present, schema order).
inline nlohmann::json RowToEvent(DataChunk &chunk, idx_t row) {
	nlohmann::json event;
	auto set_str = [&](const char *key, idx_t col) {
		auto v = chunk.GetValue(col, row);
		if (!v.IsNull()) {
			event[key] = v.ToString();
		}
	};
	set_str(EVENT_COL_SUMMARY.Name(), 1);
	set_str(EVENT_COL_DESCRIPTION.Name(), 2);
	set_str(EVENT_COL_LOCATION.Name(), 3);
	set_str(EVENT_COL_STATUS.Name(), 4);

	auto all_day_v = chunk.GetValue(10, row);
	bool all_day = !all_day_v.IsNull() && BooleanValue::Get(all_day_v);
	auto start_v = chunk.GetValue(8, row);
	auto end_v = chunk.GetValue(9, row);
	if (!start_v.IsNull()) {
		event[EVENT_COL_START_AT.Name()] = TimeNode(start_v, all_day);
	}
	if (!end_v.IsNull()) {
		event[EVENT_COL_END_AT.Name()] = TimeNode(end_v, all_day);
	}

	SetJsonPassthrough(event, "attendees", chunk.GetValue(11, row));
	SetJsonPassthrough(event, "recurrence", chunk.GetValue(12, row));
	SetJsonPassthrough(event, "reminders", chunk.GetValue(13, row));
	SetJsonPassthrough(event, "conferenceData", chunk.GetValue(14, row));
	return event;
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

// Apply one SET column (by schema index) to an existing event during UPDATE.
inline void ApplySet(nlohmann::json &event, idx_t schema_index, const Value &val, bool all_day) {
	auto set_or_remove = [&](const char *key) {
		if (val.IsNull()) {
			event.erase(key);
		} else {
			event[key] = val.ToString();
		}
	};
	switch (schema_index) {
	case 1:
		set_or_remove(EVENT_COL_SUMMARY.Name());
		break;
	case 2:
		set_or_remove(EVENT_COL_DESCRIPTION.Name());
		break;
	case 3:
		set_or_remove(EVENT_COL_LOCATION.Name());
		break;
	case 4:
		set_or_remove(EVENT_COL_STATUS.Name());
		break;
	case 8:
		if (val.IsNull()) {
			event.erase(EVENT_COL_START_AT.Name());
		} else {
			event[EVENT_COL_START_AT.Name()] = TimeNode(val, all_day);
		}
		break;
	case 9:
		if (val.IsNull()) {
			event.erase(EVENT_COL_END_AT.Name());
		} else {
			event[EVENT_COL_END_AT.Name()] = TimeNode(val, all_day);
		}
		break;
	case 10:
		// all_day handled via the all_day flag; re-encode existing nodes if start/end were not also SET.
		ReencodeTimeNode(event, EVENT_COL_START_AT.Name(), all_day);
		ReencodeTimeNode(event, EVENT_COL_END_AT.Name(), all_day);
		break;
	case 11:
		SetJsonPassthrough(event, "attendees", val);
		break;
	case 12:
		SetJsonPassthrough(event, "recurrence", val);
		break;
	case 13:
		SetJsonPassthrough(event, "reminders", val);
		break;
	case 14:
		SetJsonPassthrough(event, "conferenceData", val);
		break;
	default:
		// 0 id, 5 html_link, 6 created, 7 updated are server-managed
		throw InvalidInputException("google_calendar: column at index %llu is read-only and cannot be updated",
		                            (unsigned long long)schema_index);
	}
}

} // namespace gcal_map
} // namespace duckdb
