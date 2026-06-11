#include "calendar/util/secret.hpp"

#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {
namespace gcal {

const SecretMatch GetSecretMatch(ClientContext &ctx, const std::string &path, const std::string &type) {
	auto &manager = SecretManager::Get(ctx);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(ctx);
	return manager.LookupSecret(transaction, path, type);
}

} // namespace gcal
} // namespace duckdb
