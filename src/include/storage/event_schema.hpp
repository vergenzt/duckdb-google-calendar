#pragma once

#include "duckdb/parser/column_list.hpp"

namespace duckdb {

// Populates the fixed events schema (D6): scalar VARCHAR fields, start/end TIMESTAMP WITH TIME ZONE,
// all_day BOOLEAN, and attendees/recurrence/reminders/conference_data as VARCHAR raw-JSON passthrough.
void AddEventsColumns(ColumnList &columns);

} // namespace duckdb
