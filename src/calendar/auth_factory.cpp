#include "calendar/auth_factory.hpp"

#include "calendar/util/secret.hpp"
#include "calendar/auth/bearer_token_auth.hpp"
#include "calendar/auth/service_account_auth.hpp"

#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {
namespace gcal {

static std::unique_ptr<IAuthProvider> BuildProvider(const KeyValueSecret &kv, IHttpClient &http) {
	auto provider = kv.GetProvider();
	if (provider == "key_file") {
		Value emailValue, keyValue;
		if (!kv.TryGetValue("email", emailValue)) {
			throw InvalidInputException("'email' not found in google_calendar secret");
		}
		if (!kv.TryGetValue("secret", keyValue)) {
			throw InvalidInputException("'secret' not found in google_calendar secret");
		}
		return make_uniq<ServiceAccountAuth>(http, emailValue.ToString(), keyValue.ToString());
	}
	Value tokenValue;
	if (!kv.TryGetValue("token", tokenValue)) {
		throw InvalidInputException("'token' not found in google_calendar secret");
	}
	return make_uniq<BearerTokenAuth>(tokenValue.ToString());
}

std::unique_ptr<IAuthProvider> CreateAuthFromSecret(ClientContext &ctx, IHttpClient &http,
                                                    const std::string &secret_name) {
	if (!secret_name.empty()) {
		auto &manager = SecretManager::Get(ctx);
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(ctx);
		auto entry = manager.GetSecretByName(transaction, secret_name);
		if (!entry || !entry->secret) {
			throw InvalidInputException("google_calendar secret '%s' not found", secret_name);
		}
		auto kv = dynamic_cast<const KeyValueSecret *>(entry->secret.get());
		if (!kv) {
			throw InvalidInputException("Secret '%s' is not a google_calendar secret", secret_name);
		}
		return BuildProvider(*kv, http);
	}

	auto match = GetSecretMatch(ctx, "google_calendar", "google_calendar");
	if (match.HasMatch()) {
		auto kv = dynamic_cast<const KeyValueSecret *>(&match.GetSecret());
		return BuildProvider(*kv, http);
	}
	return nullptr;
}

} // namespace gcal
} // namespace duckdb
