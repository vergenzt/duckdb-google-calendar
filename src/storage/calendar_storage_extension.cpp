#include "storage/calendar_storage_extension.hpp"
#include "storage/calendar_catalog.hpp"
#include "storage/calendar_transaction_manager.hpp"

#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

static unique_ptr<Catalog> CalendarAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AttachOptions &options) {
	string secret_name;
	auto secret_entry = info.options.find("secret");
	if (secret_entry != info.options.end()) {
		secret_name = StringValue::Get(secret_entry->second);
	}

	auto catalog = make_uniq<CalendarCatalog>(db, info.path, secret_name);
	catalog->LoadCatalog(context);
	return std::move(catalog);
}

static unique_ptr<TransactionManager> CalendarCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                       AttachedDatabase &db, Catalog &catalog) {
	auto &calendar_catalog = catalog.Cast<CalendarCatalog>();
	return make_uniq<CalendarTransactionManager>(db, calendar_catalog);
}

CalendarStorageExtension::CalendarStorageExtension() {
	attach = CalendarAttach;
	create_transaction_manager = CalendarCreateTransactionManager;
}

} // namespace duckdb
