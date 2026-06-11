#pragma once

#include "duckdb/transaction/transaction.hpp"

#include "calendar/client.hpp"
#include "calendar/transport/http_client.hpp"
#include "calendar/auth/auth_provider.hpp"

#include <memory>

namespace duckdb {
class CalendarCatalog;

class CalendarTransaction : public Transaction {
public:
	CalendarTransaction(CalendarCatalog &catalog, TransactionManager &manager, ClientContext &context);
	~CalendarTransaction() override;

	static CalendarTransaction &Get(ClientContext &context, Catalog &catalog);

	// Lazily builds the per-statement HTTP client + auth from the catalog's secret.
	gcal::GoogleCalendarClient &GetClient(ClientContext &context);

private:
	CalendarCatalog &calendar_catalog;
	std::unique_ptr<gcal::IHttpClient> http;
	std::unique_ptr<gcal::IAuthProvider> auth;
	std::unique_ptr<gcal::GoogleCalendarClient> client;
};

} // namespace duckdb
