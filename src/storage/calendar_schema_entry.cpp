#include "storage/calendar_schema_entry.hpp"
#include "storage/calendar_catalog.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_transaction.hpp"

#include "calendar/client.hpp"

#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"

namespace duckdb {

CalendarSchemaEntry::CalendarSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info) {
}

void CalendarSchemaEntry::AddTable(unique_ptr<CatalogEntry> table) {
	tables[table->name] = std::move(table);
}

void CalendarSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	for (auto &entry : tables) {
		callback(*entry.second);
	}
}

void CalendarSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	for (auto &entry : tables) {
		callback(*entry.second);
	}
}

optional_ptr<CatalogEntry> CalendarSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                            const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto entry = tables.find(lookup_info.GetEntryName());
	if (entry == tables.end()) {
		return nullptr;
	}
	return entry->second.get();
}

static optional_ptr<CatalogEntry> RejectDDL() {
	throw NotImplementedException("google_calendar catalog is read/write for events only; DDL is not supported");
}

optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	// CTAS is handled in CalendarCatalog::PlanCreateTableAs; this path is the column-list form.
	throw NotImplementedException(
	    "google_calendar: create a calendar with CREATE TABLE <name> AS <query> "
	    "(e.g. AS FROM <existing_calendar> WHERE false for an empty one); "
	    "CREATE TABLE with an explicit column list is not supported");
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &,
                                                            TableCatalogEntry &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	return RejectDDL();
}
void CalendarSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	if (info.type != CatalogType::TABLE_ENTRY) {
		throw NotImplementedException("google_calendar catalog supports DROP TABLE only");
	}
	auto entry = tables.find(info.name);
	if (entry == tables.end()) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Table \"%s\" not found in google_calendar catalog", info.name);
	}
	auto &table = entry->second->Cast<CalendarTableEntry>();
	auto &catalog = ParentCatalog().Cast<CalendarCatalog>();
	auto &calendar_id = table.GetCalendarId();

	// Guard: only calendars this session created (via CREATE TABLE ... AS) may be dropped, so a test
	// can clean up after itself but a stray DROP can never delete a pre-existing user calendar.
	if (!catalog.WasCreatedThisSession(calendar_id)) {
		throw InvalidInputException(
		    "google_calendar: refusing to DROP \"%s\": only calendars created in this session with "
		    "CREATE TABLE ... AS can be dropped (this protects pre-existing calendars from deletion)",
		    info.name);
	}

	// Permanently delete the calendar in Google, then unregister the table.
	auto &transaction = CalendarTransaction::Get(context, catalog);
	transaction.GetClient(context).Calendars().Delete(calendar_id);
	catalog.ForgetCreatedThisSession(calendar_id);
	tables.erase(entry);
}
void CalendarSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	throw NotImplementedException("google_calendar catalog is read/write for events only; ALTER is not supported");
}

} // namespace duckdb
