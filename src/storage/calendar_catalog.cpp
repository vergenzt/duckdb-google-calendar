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

	string page_token;
	do {
		auto response = client.CalendarList().List(page_token);
		for (auto &cal : response.items) {
			// Calendar IDs are globally unique, so they serve directly as table names.
			CreateTableInfo info(*main_schema, cal.id);
			AddEventsColumns(info.columns);
			auto entry = make_uniq<CalendarTableEntry>(*this, *main_schema, info, cal.id);
			main_schema->AddTable(std::move(entry));
		}
		page_token = response.nextPageToken;
	} while (!page_token.empty());
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
	throw NotImplementedException("google_calendar catalog does not support CREATE TABLE AS");
}

PhysicalOperator &CalendarCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                              optional_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw NotImplementedException("google_calendar INSERT does not support RETURNING");
	}
	auto &table = op.table.Cast<CalendarTableEntry>();
	reference<PhysicalOperator> child = *plan;
	if (!op.column_index_map.empty()) {
		child = planner.ResolveDefaultsProjection(op, *plan);
	}
	auto &insert = planner.Make<CalendarInsert>(op.types, table, op.estimated_cardinality);
	insert.children.push_back(child);
	return insert;
}

PhysicalOperator &CalendarCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                              PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw NotImplementedException("google_calendar DELETE does not support RETURNING");
	}
	auto &table = op.table.Cast<CalendarTableEntry>();
	auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
	auto &del = planner.Make<CalendarDelete>(op.types, table, bound_ref.index, op.estimated_cardinality);
	del.children.push_back(plan);
	return del;
}

PhysicalOperator &CalendarCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                              PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw NotImplementedException("google_calendar UPDATE does not support RETURNING");
	}
	auto &table = op.table.Cast<CalendarTableEntry>();
	vector<idx_t> value_indices;
	for (auto &expr : op.expressions) {
		value_indices.push_back(expr->Cast<BoundReferenceExpression>().index);
	}
	auto &update =
	    planner.Make<CalendarUpdate>(op.types, table, op.columns, std::move(value_indices), op.estimated_cardinality);
	update.children.push_back(plan);
	return update;
}

PhysicalOperator &CalendarCatalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
                                                 LogicalMergeInto &op, PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw NotImplementedException("google_calendar MERGE does not support RETURNING");
	}
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
