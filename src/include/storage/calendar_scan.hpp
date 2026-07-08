#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/constants.hpp"

namespace duckdb {

// Virtual (hidden) column exposing the owning calendar's id. Excluded from SELECT *, but selectable
// by name — handy for tagging rows when merging across calendars. Must be >= VIRTUAL_COLUMN_START.
static const column_t CALENDAR_ID_VIRTUAL_COLUMN = VIRTUAL_COLUMN_START;

TableFunction GetCalendarScanFunction();

unique_ptr<FunctionData> MakeCalendarScanBindData(Catalog &catalog, TableCatalogEntry &table, string calendar_id,
                                                  vector<string> names, vector<LogicalType> types);

} // namespace duckdb
