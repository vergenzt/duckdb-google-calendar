#pragma once

#include <memory>

#include "duckdb/main/client_context.hpp"

#include "calendar/transport/http_client.hpp"

namespace duckdb {
namespace gcal {

std::unique_ptr<IHttpClient> CreateHttpClient(ClientContext &ctx);

} // namespace gcal
} // namespace duckdb
