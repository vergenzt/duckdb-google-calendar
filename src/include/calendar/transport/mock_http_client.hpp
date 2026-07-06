#pragma once

#include <vector>

#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

class MockHttpClient : public IHttpClient {
public:
	HttpResponse Execute(const HttpRequest &request) override;
	void AddResponse(HttpResponse response);

private:
	size_t responseIndex = 0;
	std::vector<HttpResponse> responses;
};
} // namespace gcal
} // namespace duckdb
