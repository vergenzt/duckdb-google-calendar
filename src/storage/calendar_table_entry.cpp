#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_scan.hpp"

#include "duckdb/common/table_column.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

unique_ptr<BaseStatistics> CalendarTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
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

vector<string> CalendarTableEntry::ColumnNames() const {
	vector<string> names;
	for (auto &col : GetColumns().Logical()) {
		names.push_back(col.Name());
	}
	return names;
}

EventsTableEntry::EventsTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
    : CalendarTableEntry(catalog, schema, info) {
}

TableFunction EventsTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	bind_data = MakeEventsScanBindData(ParentCatalog(), *this, ColumnNames());
	return GetEventsScanFunction();
}

CalendarsTableEntry::CalendarsTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
    : CalendarTableEntry(catalog, schema, info) {
}

TableFunction CalendarsTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	bind_data = MakeCalendarsScanBindData(ParentCatalog(), *this, ColumnNames());
	return GetCalendarsScanFunction();
}

} // namespace duckdb
