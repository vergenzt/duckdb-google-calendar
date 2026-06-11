#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class CalendarTableEntry : public TableCatalogEntry {
public:
	CalendarTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, string calendar_id);

	// The stable Google Calendar id (e.g. "primary" or "...@group.calendar.google.com").
	const string &GetCalendarId() const {
		return calendar_id;
	}

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	// Rowid = the event id (VARCHAR), not the conventional int64 row_t (D7).
	vector<column_t> GetRowIdColumns() const override;
	virtual_column_map_t GetVirtualColumns() const override;

private:
	string calendar_id;
};

} // namespace duckdb
