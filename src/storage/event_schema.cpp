#include "storage/event_schema.hpp"

#include "duckdb/parser/column_definition.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

void AddEventsColumns(ColumnList &columns) {
	// Owning calendar's id. Not a Google event field: the scan fills it from the calendar it is
	// currently paging, and it is half of the row identity that routes UPDATE/DELETE/MERGE. Required
	// on INSERT, since it is what selects the destination calendar.
	columns.AddColumn(ColumnDefinition("calendar_id", LogicalType::VARCHAR));

	// Core scalar fields.
	columns.AddColumn(ColumnDefinition("event_id", LogicalType::VARCHAR));
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

	// Classification / display.
	columns.AddColumn(ColumnDefinition("color_id", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("transparency", LogicalType::VARCHAR)); // opaque (busy) / transparent (free)
	columns.AddColumn(ColumnDefinition("visibility", LogicalType::VARCHAR));   // default/public/private/confidential
	columns.AddColumn(ColumnDefinition("event_type", LogicalType::VARCHAR));

	// Recurrence / identity.
	columns.AddColumn(ColumnDefinition("recurring_event_id", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("original_start_time", LogicalType::TIMESTAMP_TZ));
	columns.AddColumn(ColumnDefinition("ical_uid", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("sequence", LogicalType::BIGINT));
	columns.AddColumn(ColumnDefinition("etag", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("kind", LogicalType::VARCHAR));

	// Links / people.
	columns.AddColumn(ColumnDefinition("hangout_link", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("creator", LogicalType::VARCHAR));   // raw-JSON passthrough
	columns.AddColumn(ColumnDefinition("organizer", LogicalType::VARCHAR)); // raw-JSON passthrough

	// Flags.
	columns.AddColumn(ColumnDefinition("end_time_unspecified", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("attendees_omitted", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("anyone_can_add_self", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("guests_can_invite_others", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("guests_can_modify", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("guests_can_see_other_guests", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("private_copy", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("locked", LogicalType::BOOLEAN));

	// Structured fields as raw-JSON passthrough VARCHAR.
	columns.AddColumn(ColumnDefinition("attendees", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("recurrence", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("reminders", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("conference_data", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("extended_properties", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("source", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("attachments", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("working_location_properties", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("out_of_office_properties", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("focus_time_properties", LogicalType::VARCHAR));
}

void AddCalendarsColumns(ColumnList &columns) {
	// Identity. Named calendar_id (not id) so `events JOIN calendars USING (calendar_id)` works.
	columns.AddColumn(ColumnDefinition("calendar_id", LogicalType::VARCHAR));

	// Writable on INSERT (calendars.insert accepts these four).
	columns.AddColumn(ColumnDefinition("summary", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("description", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("location", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("time_zone", LogicalType::VARCHAR));

	// Per-user calendarList overlay (read-only).
	columns.AddColumn(ColumnDefinition("summary_override", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("access_role", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("color_id", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("background_color", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("foreground_color", LogicalType::VARCHAR));

	// Flags. `primary` is a SQL keyword, so the column is is_primary to keep it quote-free.
	columns.AddColumn(ColumnDefinition("is_primary", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("selected", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("hidden", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("deleted", LogicalType::BOOLEAN));

	columns.AddColumn(ColumnDefinition("etag", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("kind", LogicalType::VARCHAR));
}

} // namespace duckdb
