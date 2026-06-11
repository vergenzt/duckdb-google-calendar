#pragma once

#include "duckdb/main/client_context.hpp"

#include "calendar/auth/auth_provider.hpp"
#include "calendar/transport/http_client.hpp"

namespace duckdb {
namespace gcal {

// secret_name: when non-empty, resolve that named secret; otherwise use the default google_calendar secret.
std::unique_ptr<IAuthProvider> CreateAuthFromSecret(ClientContext &ctx, IHttpClient &http,
                                                    const std::string &secret_name = "");

} // namespace gcal
} // namespace duckdb
