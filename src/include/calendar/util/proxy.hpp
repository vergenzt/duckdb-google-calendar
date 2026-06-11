#pragma once

#include "duckdb/main/client_context.hpp"

#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

HttpProxyConfig GetHttpProxyConfig(ClientContext &ctx);

} // namespace gcal
} // namespace duckdb
