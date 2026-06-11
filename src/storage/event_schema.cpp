#include "storage/event_schema.hpp"

#include "duckdb/parser/column_definition.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

void AddEventsColumns(ColumnList &columns) {
	columns.AddColumn(ColumnDefinition("id", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("summary", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("description", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("location", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("status", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("html_link", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("created", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("updated", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("start", LogicalType::TIMESTAMP_TZ));
	columns.AddColumn(ColumnDefinition("end", LogicalType::TIMESTAMP_TZ));
	columns.AddColumn(ColumnDefinition("all_day", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("attendees", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("recurrence", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("reminders", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("conference_data", LogicalType::VARCHAR));
}

} // namespace duckdb
