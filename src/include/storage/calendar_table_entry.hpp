#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Base for the catalog's two fixed tables (D15). Both use a VARCHAR rowid rather than the
// conventional int64 row_t (D7); what the rowid carries differs per table.
class CalendarTableEntry : public TableCatalogEntry {
public:
	CalendarTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
	    : TableCatalogEntry(catalog, schema, info) {
	}

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	vector<column_t> GetRowIdColumns() const override;
	virtual_column_map_t GetVirtualColumns() const override;

protected:
	// Column names in logical order; every scan and DML sink keys off names, not positions.
	vector<string> ColumnNames() const;
};

// `events`: every event on every calendar the credential can see. Rowid = the packed
// "<calendar_id>\x1f<event_id>" (gcal_map::PackRowId), since an event id alone does not say which
// calendar to issue the API call against.
class EventsTableEntry : public CalendarTableEntry {
public:
	EventsTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
};

// `calendars`: the entries of users/me/calendarList. Rowid = the calendar id.
class CalendarsTableEntry : public CalendarTableEntry {
public:
	CalendarsTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
};

} // namespace duckdb
