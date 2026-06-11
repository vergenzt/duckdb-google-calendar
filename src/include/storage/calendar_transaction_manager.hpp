#pragma once

#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/common/mutex.hpp"

#include <vector>

namespace duckdb {
class CalendarCatalog;
class CalendarTransaction;

class CalendarTransactionManager : public TransactionManager {
public:
	CalendarTransactionManager(AttachedDatabase &db, CalendarCatalog &catalog);
	~CalendarTransactionManager() override;

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	CalendarCatalog &calendar_catalog;
	mutex transaction_lock;
	vector<unique_ptr<CalendarTransaction>> transactions;
};

} // namespace duckdb
