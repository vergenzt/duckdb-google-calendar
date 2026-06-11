#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_scan.hpp"

#include "duckdb/common/table_column.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

CalendarTableEntry::CalendarTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                       string calendar_id)
    : TableCatalogEntry(catalog, schema, info), calendar_id(std::move(calendar_id)) {
}

unique_ptr<BaseStatistics> CalendarTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction CalendarTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	vector<string> names;
	vector<LogicalType> types;
	for (auto &col : GetColumns().Logical()) {
		names.push_back(col.Name());
		types.push_back(col.Type());
	}
	bind_data = MakeCalendarScanBindData(ParentCatalog(), calendar_id, std::move(names), std::move(types));
	return GetCalendarScanFunction();
}

TableStorageInfo CalendarTableEntry::GetStorageInfo(ClientContext &context) {
	return TableStorageInfo();
}

vector<column_t> CalendarTableEntry::GetRowIdColumns() const {
	vector<column_t> result;
	result.push_back(COLUMN_IDENTIFIER_ROW_ID);
	return result;
}

virtual_column_map_t CalendarTableEntry::GetVirtualColumns() const {
	virtual_column_map_t virtual_columns;
	virtual_columns.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::VARCHAR)));
	return virtual_columns;
}

} // namespace duckdb
