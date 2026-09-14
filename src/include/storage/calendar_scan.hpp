#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/constants.hpp"

namespace duckdb {

// `events`: fans out over the calendars in users/me/calendarList, paging events.list per calendar.
// A `calendar_id = ...` / `calendar_id IN (...)` predicate is pushed down to narrow the fan-out; a
// `start` bound is pushed down as timeMin/timeMax. Both are hints — the residual filter is retained.
TableFunction GetEventsScanFunction();
unique_ptr<FunctionData> MakeEventsScanBindData(Catalog &catalog, TableCatalogEntry &table, vector<string> names);

// `calendars`: a single users/me/calendarList enumeration.
TableFunction GetCalendarsScanFunction();
unique_ptr<FunctionData> MakeCalendarsScanBindData(Catalog &catalog, TableCatalogEntry &table, vector<string> names);

} // namespace duckdb
