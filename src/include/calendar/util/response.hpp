#pragma once

#include "json.hpp"

#include "calendar/exception.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

template <typename T>
T ParseResponse(const HttpResponse &response) {
	if (response.statusCode != 200) {
		throw CalendarApiException(response.statusCode, response.body);
	}
	try {
		return nlohmann::json::parse(response.body).get<T>();
	} catch (const nlohmann::json::exception &e) {
		throw CalendarParseException("Failed to parse response: " + std::string(e.what()));
	}
}

} // namespace gcal
} // namespace duckdb
