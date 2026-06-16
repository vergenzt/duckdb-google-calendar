#pragma once

#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb {

struct EventColumns {
	ColumnDefinition ID = {"event_id", LogicalType::VARCHAR}; // done
	ColumnDefinition SUMMARY = {"summary", LogicalType::VARCHAR};
	ColumnDefinition DESCRIPTION = {"description", LogicalType::VARCHAR};
	ColumnDefinition LOCATION = {"location", LogicalType::VARCHAR};
	ColumnDefinition STATUS = {"status", LogicalType::VARCHAR};
	ColumnDefinition HTML_URL = {"html_link", LogicalType::VARCHAR};
	ColumnDefinition CREATED_AT = {"created", LogicalType::VARCHAR};
	ColumnDefinition UPDATED_AT = {"updated", LogicalType::VARCHAR};
	ColumnDefinition START_AT = {"start", LogicalType::TIMESTAMP_TZ};
	ColumnDefinition END_AT = {"end", LogicalType::TIMESTAMP_TZ};
	ColumnDefinition ALL_DAY = {"all_day", LogicalType::BOOLEAN};
	ColumnDefinition ATTENDEES = {"attendees_json", LogicalType::VARCHAR};
	ColumnDefinition RECURRENCE = {"recurrence_json", LogicalType::VARCHAR};
	ColumnDefinition REMINDERS = {"reminders_json", LogicalType::VARCHAR};
	ColumnDefinition CONFERENCE_DATA = {"conference_data_json", LogicalType::VARCHAR};

	constexpr ColumnDefinition& operator[](size_t index) {
            switch (index) {
                case 0: return &ID;
                case 1: return &SUMMARY;
                case 2: return &DESCRIPTION;

ID
SUMMARY
DESCRIPTION
LOCATION
STATUS
HTML_URL
CREATED_AT
UPDATED_AT
START_AT
END_AT
ALL_DAY
ATTENDEES
RECURRENCE
REMINDERS
CONFERENCE_DATA

 default: throw std::out_of_range("index");
            }
    }
};

static const EventColumns EVENT_COLUMNS;


} // namespace duckdb
