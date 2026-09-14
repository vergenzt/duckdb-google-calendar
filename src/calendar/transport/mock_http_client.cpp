#include "calendar/transport/mock_http_client.hpp"
#include <stdexcept>

namespace duckdb {
namespace gcal {

HttpResponse MockHttpClient::Execute(const HttpRequest &request) {
	std::lock_guard<std::mutex> guard(lock);
	if (responseIndex < responses.size()) {
		return responses[responseIndex++];
	}
	throw std::runtime_error("MockHttpClient: No more responses queued");
}

void MockHttpClient::AddResponse(HttpResponse response) {
	std::lock_guard<std::mutex> guard(lock);
	responses.push_back(std::move(response));
}
} // namespace gcal
} // namespace duckdb
