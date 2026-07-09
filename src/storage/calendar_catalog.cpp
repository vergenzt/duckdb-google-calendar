#include "storage/calendar_catalog.hpp"
#include "storage/calendar_schema_entry.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/event_schema.hpp"
#include "storage/calendar_insert.hpp"
#include "storage/calendar_update.hpp"
#include "storage/calendar_delete.hpp"

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
	// Schema container is built in the constructor; per-calendar tables are added by LoadCatalog.
}

void CalendarCatalog::LoadCatalog(ClientContext &context) {
	auto http = gcal::CreateHttpClient(context);
	auto auth = gcal::CreateAuthFromSecret(context, *http, secret_name);
	if (!auth) {
		throw InvalidInputException(
		    "No google_calendar secret found. Create one with CREATE SECRET (TYPE google_calendar, ...) "
		    "or pass SECRET <name> to ATTACH.");
	}
	gcal::GoogleCalendarClient client(*http, *auth);

	case_insensitive_set_t aliased_ids;
	string page_token;
	do {
		auto response = client.CalendarList().List(page_token);
		for (auto &cal : response.items) {
			// Calendar IDs are globally unique, so they serve directly as table names, unless an
			// alias was supplied at ATTACH, in which case the calendar is mounted only under it.
			auto alias = calendar_aliases.find(cal.id);
			auto table_name = alias != calendar_aliases.end() ? alias->second : cal.id;
			if (alias != calendar_aliases.end()) {
				aliased_ids.insert(cal.id);
			}
			CreateTableInfo info(*main_schema, table_name);
			AddEventsColumns(info.columns);
			auto entry = make_uniq<CalendarTableEntry>(*this, *main_schema, info, cal.id);
			main_schema->AddTable(std::move(entry));
		}
		page_token = response.nextPageToken;
	} while (!page_token.empty());

	// Fail eagerly if an alias points at a calendar ID that doesn't exist.
	for (auto &alias : calendar_aliases) {
		if (aliased_ids.find(alias.first) == aliased_ids.end()) {
			throw InvalidInputException("calendar_aliases refers to unknown calendar ID \"%s\"", alias.first);
		}
	}
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
	auto &create_info = op.info->Base();

	// The events schema is fixed, so the query must yield exactly those columns.
	ValidateEventsColumns(create_info.columns);

	// Create the calendar in Google (POST /calendars); the table name becomes its summary.
	auto &transaction = CalendarTransaction::Get(context, *this);
	auto created = transaction.GetClient(context).Calendars().Insert(create_info.table);

	// Register it as a live table so it's queryable without re-ATTACH.
	CreateTableInfo info(*main_schema, create_info.table);
	AddEventsColumns(info.columns);
	auto entry = make_uniq<CalendarTableEntry>(*this, *main_schema, info, created.id);
	auto &table_entry = *entry;
	main_schema->AddTable(std::move(entry));
	MarkCreatedThisSession(created.id);

	// Insert the query's rows into the new calendar (none for `... AS FROM x WHERE false`).
	auto &insert = planner.Make<CalendarInsert>(op.types, table_entry, op.estimated_cardinality, false);
	insert.children.push_back(plan);
	return insert;
}

PhysicalOperator &CalendarCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                              optional_ptr<PhysicalOperator> plan) {
	auto &table = op.table.Cast<CalendarTableEntry>();
	reference<PhysicalOperator> child = *plan;
	if (!op.column_index_map.empty()) {
		child = planner.ResolveDefaultsProjection(op, *plan);
	}
	auto &insert = planner.Make<CalendarInsert>(op.types, table, op.estimated_cardinality, op.return_chunk);
	insert.children.push_back(child);
	return insert;
}

PhysicalOperator &CalendarCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                              PhysicalOperator &plan) {
	auto &table = op.table.Cast<CalendarTableEntry>();
	auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
	auto &del = planner.Make<CalendarDelete>(op.types, table, bound_ref.index, op.estimated_cardinality, op.return_chunk);
	del.children.push_back(plan);
	return del;
}

PhysicalOperator &CalendarCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                              PhysicalOperator &plan) {
	auto &table = op.table.Cast<CalendarTableEntry>();
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
