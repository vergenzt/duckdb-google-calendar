#pragma once

#include "duckdb/parser/column_list.hpp"

namespace duckdb {

// Populates the fixed events schema (D6): scalar VARCHAR fields, start/end TIMESTAMP WITH TIME ZONE,
// all_day BOOLEAN, and attendees/recurrence/reminders/conference_data as VARCHAR raw-JSON passthrough.
// `calendar_id` leads the list: it identifies the owning calendar, is part of row identity, and is
// required on INSERT (D15).
void AddEventsColumns(ColumnList &columns);

// Populates the fixed `calendars` schema (D15), projected from users/me/calendarList. Only
// summary/description/location/time_zone are writable, and only on INSERT (calendars.insert).
void AddCalendarsColumns(ColumnList &columns);

} // namespace duckdb
