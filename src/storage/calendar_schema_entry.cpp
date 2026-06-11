#include "storage/calendar_schema_entry.hpp"

#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"

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
	return RejectDDL();
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
void CalendarSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	throw NotImplementedException("google_calendar catalog is read/write for events only; DROP is not supported");
}
void CalendarSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	throw NotImplementedException("google_calendar catalog is read/write for events only; ALTER is not supported");
}

} // namespace duckdb
