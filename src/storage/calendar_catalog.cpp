#include "storage/calendar_catalog.hpp"
#include "storage/calendar_schema_entry.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/event_schema.hpp"
#include "storage/calendar_insert.hpp"
#include "storage/calendar_update.hpp"
#include "storage/calendar_delete.hpp"
#include "storage/calendars_dml.hpp"

#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/operator/persistent/physical_merge_into.hpp"

#include "storage/calendar_merge.hpp"

#include "calendar/client.hpp"
#include "calendar/transport/client_factory.hpp"
#include "calendar/auth_factory.hpp"

#include "storage/calendar_transaction.hpp"

namespace duckdb {

CalendarCatalog::CalendarCatalog(AttachedDatabase &db, string path, string secret_name)
    : Catalog(db), path(std::move(path)), secret_name(std::move(secret_name)) {
	CreateSchemaInfo schema_info;
	schema_info.schema = DEFAULT_SCHEMA;
	main_schema = make_uniq<CalendarSchemaEntry>(*this, schema_info);
}

CalendarCatalog::~CalendarCatalog() = default;

void CalendarCatalog::Initialize(bool load_builtin) {
	// Schema container is built in the constructor; the two tables are added by LoadCatalog.
}

void CalendarCatalog::LoadCatalog(ClientContext &context) {
	// Resolve the credential now so a bad or missing secret fails at ATTACH rather than at the first
	// query. No calendar enumeration happens here: the table set is fixed, and each scan re-reads the
	// calendar list so newly created/shared calendars show up without re-ATTACH.
	auto http = gcal::CreateHttpClient(context);
	auto auth = gcal::CreateAuthFromSecret(context, *http, secret_name);
	if (!auth) {
		throw InvalidInputException(
		    "No google_calendar secret found. Create one with CREATE SECRET (TYPE google_calendar, ...) "
		    "or pass SECRET <name> to ATTACH.");
	}

	CreateTableInfo calendars_info(*main_schema, CALENDARS_TABLE_NAME);
	AddCalendarsColumns(calendars_info.columns);
	main_schema->AddTable(make_uniq<CalendarsTableEntry>(*this, *main_schema, calendars_info));

	CreateTableInfo events_info(*main_schema, EVENTS_TABLE_NAME);
	AddEventsColumns(events_info.columns);
	main_schema->AddTable(make_uniq<EventsTableEntry>(*this, *main_schema, events_info));
}

optional_ptr<CatalogEntry> CalendarCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	throw NotImplementedException("google_calendar catalog does not support CREATE SCHEMA");
}

optional_ptr<SchemaCatalogEntry> CalendarCatalog::LookupSchema(CatalogTransaction transaction,
                                                               const EntryLookupInfo &schema_lookup,
                                                               OnEntryNotFound if_not_found) {
	auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name.empty() || schema_name == DEFAULT_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	throw BinderException("Schema \"%s\" not found in google_calendar catalog", schema_name);
}

void CalendarCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

PhysicalOperator &CalendarCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                     LogicalCreateTable &op, PhysicalOperator &plan) {
	throw NotImplementedException(
	    "google_calendar: the catalog has a fixed table set (calendars, events); CREATE TABLE is not "
	    "supported. Create a calendar with INSERT INTO calendars (summary) VALUES ('...').");
}

PhysicalOperator &CalendarCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                              optional_ptr<PhysicalOperator> plan) {
	auto &table = op.table.Cast<CalendarTableEntry>();
	reference<PhysicalOperator> child = *plan;
	if (!op.column_index_map.empty()) {
		child = planner.ResolveDefaultsProjection(op, *plan);
	}
	if (table.name == CALENDARS_TABLE_NAME) {
		auto &insert = planner.Make<CalendarsInsert>(op.types, table, op.estimated_cardinality, op.return_chunk);
		insert.children.push_back(child);
		return insert;
	}
	auto &insert = planner.Make<CalendarInsert>(op.types, table, op.estimated_cardinality, op.return_chunk);
	insert.children.push_back(child);
	return insert;
}

PhysicalOperator &CalendarCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                              PhysicalOperator &plan) {
	auto &table = op.table.Cast<CalendarTableEntry>();
	auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
	if (table.name == CALENDARS_TABLE_NAME) {
		auto &del =
		    planner.Make<CalendarsDelete>(op.types, table, bound_ref.index, op.estimated_cardinality, op.return_chunk);
		del.children.push_back(plan);
		return del;
	}
	auto &del = planner.Make<CalendarDelete>(op.types, table, bound_ref.index, op.estimated_cardinality, op.return_chunk);
	del.children.push_back(plan);
	return del;
}

PhysicalOperator &CalendarCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                              PhysicalOperator &plan) {
	auto &table = op.table.Cast<CalendarTableEntry>();
	if (table.name == CALENDARS_TABLE_NAME) {
		// calendars.patch is a different call than the events read-modify-write PUT, and the calendarList
		// overlay fields (selected/hidden/colors) live on a third endpoint again.
		throw NotImplementedException("google_calendar: UPDATE on the calendars table is not supported; "
		                              "the table is read-only apart from INSERT (create) and DELETE (remove)");
	}
	vector<idx_t> value_indices;
	for (auto &expr : op.expressions) {
		value_indices.push_back(expr->Cast<BoundReferenceExpression>().index);
	}
	auto &update = planner.Make<CalendarUpdate>(op.types, table, op.columns, std::move(value_indices),
	                                            op.estimated_cardinality, op.return_chunk);
	update.children.push_back(plan);
	return update;
}

PhysicalOperator &CalendarCatalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
                                                 LogicalMergeInto &op, PhysicalOperator &plan) {
	auto &table = op.table.Cast<CalendarTableEntry>();
	if (table.name == CALENDARS_TABLE_NAME) {
		throw NotImplementedException("google_calendar: MERGE INTO the calendars table is not supported; "
		                              "use INSERT INTO calendars / DELETE FROM calendars");
	}

	map<MergeActionCondition, vector<unique_ptr<MergeIntoOperator>>> actions;
	for (auto &entry : op.actions) {
		vector<unique_ptr<MergeIntoOperator>> planned_actions;
		for (auto &action : entry.second) {
			planned_actions.push_back(PlanCalendarMergeIntoAction(context, op, planner, table, *action));
		}
		actions.emplace(entry.first, std::move(planned_actions));
	}

	auto &result = planner.Make<PhysicalMergeInto>(op.types, std::move(actions), op.row_id_start, op.source_marker,
	                                               false, op.return_chunk);
	result.children.push_back(plan);
	return result;
}

DatabaseSize CalendarCatalog::GetDatabaseSize(ClientContext &context) {
	return DatabaseSize();
}

bool CalendarCatalog::InMemory() {
	return false;
}

string CalendarCatalog::GetDBPath() {
	return path;
}

void CalendarCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw NotImplementedException("google_calendar catalog does not support DROP SCHEMA");
}

} // namespace duckdb
