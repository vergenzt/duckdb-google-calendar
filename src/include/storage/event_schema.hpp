#pragma once

#include "duckdb/parser/column_list.hpp"

namespace duckdb {

// Populates the fixed events schema (D6): scalar VARCHAR fields, start/end TIMESTAMP WITH TIME ZONE,
// all_day BOOLEAN, and attendees/recurrence/reminders/conference_data as VARCHAR raw-JSON passthrough.
void AddEventsColumns(ColumnList &columns);

// Throws BinderException unless `columns` matches the fixed events schema exactly (same names, order,
// and types). Used to reject CREATE TABLE shapes that wouldn't round-trip as calendar events.
void ValidateEventsColumns(const ColumnList &columns);

} // namespace duckdb
