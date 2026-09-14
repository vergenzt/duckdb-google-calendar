#pragma once

#include "duckdb/transaction/transaction.hpp"

#include "calendar/client.hpp"
#include "calendar/transport/http_client.hpp"
#include "calendar/auth/auth_provider.hpp"

#include <memory>
#include <mutex>

namespace duckdb {
class CalendarCatalog;

class CalendarTransaction : public Transaction {
public:
	CalendarTransaction(CalendarCatalog &catalog, TransactionManager &manager, ClientContext &context);
	~CalendarTransaction() override;

	static CalendarTransaction &Get(ClientContext &context, Catalog &catalog);

	// Lazily builds the per-statement HTTP client + auth from the catalog's secret. Safe to call from
	// several executor threads at once: a single statement can init this from more than one pipeline
	// (e.g. a DELETE whose filter carries a correlated subquery), and the members below are built once
	// and then only read.
	gcal::GoogleCalendarClient &GetClient(ClientContext &context);

private:
	CalendarCatalog &calendar_catalog;
	// Guards the one-time construction of http/auth/client below -- not their later use, which is
	// read-only (HttpLibClient builds a fresh httplib::Client per Execute, and the auth token is
	// baked into the client's headers at construction).
	std::mutex client_lock;
	std::unique_ptr<gcal::IHttpClient> http;
	std::unique_ptr<gcal::IAuthProvider> auth;
	std::unique_ptr<gcal::GoogleCalendarClient> client;
};

} // namespace duckdb
