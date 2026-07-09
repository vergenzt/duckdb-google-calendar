#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {
class CalendarSchemaEntry;

class CalendarCatalog : public Catalog {
public:
	CalendarCatalog(AttachedDatabase &db, string path, string secret_name);
	~CalendarCatalog() override;

	const string &GetSecretName() const {
		return secret_name;
	}

	// Optional map of calendar ID -> table name. A calendar whose ID is a key here is mounted only
	// under its alias; every other calendar is mounted under its raw ID.
	case_insensitive_map_t<string> calendar_aliases;

	// Builds per-calendar tables via the Calendar API (enumeration body lands in Slice 6).
	void LoadCatalog(ClientContext &context);

	// Calendars created via CREATE TABLE ... AS during this attachment. DROP TABLE is allowed only for
	// these, so a session can clean up after itself but never delete pre-existing user calendars.
	void MarkCreatedThisSession(const string &calendar_id) {
		created_calendar_ids.insert(calendar_id);
	}
	bool WasCreatedThisSession(const string &calendar_id) const {
		return created_calendar_ids.find(calendar_id) != created_calendar_ids.end();
	}
	void ForgetCreatedThisSession(const string &calendar_id) {
		created_calendar_ids.erase(calendar_id);
	}

	// Optional default time window, set via ATTACH with any two of {default_window_start,
	// default_window_end, default_window_length}, or default_window_length alone. When set, a scan
	// with no explicit `start` bound uses this window instead of erroring — which is what makes
	// MERGE and unqualified UPDATE/DELETE reachable (their target scan carries no `start` filter).
	// Two modes:
	//   dynamic (length alone): [now - length/2, now + length/2], recomputed per query.
	//   static  (a fixed pair): the absolute [default_window_start, default_window_end] resolved here.
	bool has_default_window = false;
	bool default_is_dynamic = false;
	interval_t default_window_length {};                           // dynamic: full width of the window
	timestamp_tz_t default_window_start {}, default_window_end {}; // static: absolute bounds

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return "google_calendar";
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner, LogicalMergeInto &op,
	                                PhysicalOperator &plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override;
	string GetDBPath() override;

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	string path;
	string secret_name;
	unique_ptr<CalendarSchemaEntry> main_schema;
	case_insensitive_set_t created_calendar_ids;
};

} // namespace duckdb
