#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

TableFunction GetCalendarScanFunction();

unique_ptr<FunctionData> MakeCalendarScanBindData(Catalog &catalog, TableCatalogEntry &table, string calendar_id,
                                                  vector<string> names, vector<LogicalType> types);

} // namespace duckdb
