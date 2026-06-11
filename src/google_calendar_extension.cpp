#define DUCKDB_EXTENSION_MAIN

#include "google_calendar_extension.hpp"
#include "duckdb.hpp"

#include "calendar_auth.hpp"
#include "storage/calendar_storage_extension.hpp"

#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	// Initialize OpenSSL (used for RS256 JWT signing in the service-account auth path).
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();

	// Register the google_calendar secret type + providers (oauth / access_token / key_file).
	CreateGoogleCalendarSecretFunctions::Register(loader);

	// Register the StorageExtension so ATTACH ... (TYPE google_calendar) dispatches to our catalog.
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	StorageExtension::Register(config, "google_calendar", make_shared_ptr<CalendarStorageExtension>());
}

void GoogleCalendarExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string GoogleCalendarExtension::Name() {
	return "google_calendar";
}

std::string GoogleCalendarExtension::Version() const {
#ifdef EXT_VERSION_GOOGLE_CALENDAR
	return EXT_VERSION_GOOGLE_CALENDAR;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(google_calendar, loader) {
	duckdb::LoadInternal(loader);
}
}
