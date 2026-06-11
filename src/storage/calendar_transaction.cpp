#include "storage/calendar_transaction.hpp"
#include "storage/calendar_catalog.hpp"

#include "duckdb/common/exception.hpp"

#include "calendar/transport/client_factory.hpp"
#include "calendar/auth_factory.hpp"

namespace duckdb {

CalendarTransaction::CalendarTransaction(CalendarCatalog &catalog, TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), calendar_catalog(catalog) {
}

CalendarTransaction::~CalendarTransaction() = default;

CalendarTransaction &CalendarTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<CalendarTransaction>();
}

gcal::GoogleCalendarClient &CalendarTransaction::GetClient(ClientContext &context) {
	if (!client) {
		http = gcal::CreateHttpClient(context);
		auth = gcal::CreateAuthFromSecret(context, *http, calendar_catalog.GetSecretName());
		if (!auth) {
			throw InvalidInputException(
			    "No google_calendar secret found. Create one with CREATE SECRET (TYPE google_calendar, ...) "
			    "or pass SECRET <name> to ATTACH.");
		}
		client = make_uniq<gcal::GoogleCalendarClient>(*http, *auth);
	}
	return *client;
}

} // namespace duckdb
