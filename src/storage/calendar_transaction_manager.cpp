#include "storage/calendar_transaction_manager.hpp"
#include "storage/calendar_transaction.hpp"

namespace duckdb {

CalendarTransactionManager::CalendarTransactionManager(AttachedDatabase &db, CalendarCatalog &catalog)
    : TransactionManager(db), calendar_catalog(catalog) {
}

CalendarTransactionManager::~CalendarTransactionManager() = default;

Transaction &CalendarTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<CalendarTransaction>(calendar_catalog, *this, context);
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions.push_back(std::move(transaction));
	return result;
}

ErrorData CalendarTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	lock_guard<mutex> l(transaction_lock);
	for (idx_t i = 0; i < transactions.size(); i++) {
		if (transactions[i].get() == &transaction) {
			transactions.erase(transactions.begin() + i);
			break;
		}
	}
	return ErrorData();
}

void CalendarTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> l(transaction_lock);
	for (idx_t i = 0; i < transactions.size(); i++) {
		if (transactions[i].get() == &transaction) {
			transactions.erase(transactions.begin() + i);
			break;
		}
	}
}

void CalendarTransactionManager::Checkpoint(ClientContext &context, bool force) {
}

} // namespace duckdb
