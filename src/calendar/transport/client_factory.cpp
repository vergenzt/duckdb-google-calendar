#include <cstdlib>
#include <fstream>

#include "json.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

#include "calendar/transport/client_factory.hpp"
#include "calendar/transport/httplib_client.hpp"
#include "calendar/transport/mock_http_client.hpp"
#include "calendar/transport/retrying_http_client.hpp"
#include "calendar/util/proxy.hpp"

using json = nlohmann::json;

namespace duckdb {
namespace gcal {

// Test-only: load a queue of canned responses from a fixture file. Format:
//   { "responses": [ { "status": 429, "body": "..." }, { "status": 200, "body": "{...}" } ] }
static std::unique_ptr<MockHttpClient> LoadMockFromFixture(const std::string &path) {
	std::ifstream ifs(path);
	if (!ifs.is_open()) {
		throw IOException("Could not open GOOGLE_CALENDAR_TEST_FIXTURE at: " + path);
	}
	json fixture = json::parse(ifs);
	auto mock = make_uniq<MockHttpClient>();
	for (const auto &entry : fixture.at("responses")) {
		HttpResponse response;
		response.statusCode = entry.value("status", 200);
		response.body = entry.value("body", std::string());
		if (entry.contains("headers")) {
			for (auto it = entry["headers"].begin(); it != entry["headers"].end(); ++it) {
				response.headers[it.key()] = it.value().get<std::string>();
			}
		}
		mock->AddResponse(std::move(response));
	}
	return mock;
}

std::unique_ptr<IHttpClient> CreateHttpClient(ClientContext &ctx) {
	RetryConfig retry_config;

	const char *fixture = std::getenv("GOOGLE_CALENDAR_TEST_FIXTURE");
	if (fixture && *fixture) {
		// Test seam: deterministic, credential-free; no real backoff sleeps under tests.
		retry_config.zero_sleep = true;
		return make_uniq<RetryingHttpClient>(LoadMockFromFixture(fixture), retry_config);
	}

	auto proxy_config = GetHttpProxyConfig(ctx);
	auto real = make_uniq<HttpLibClient>(proxy_config);
	return make_uniq<RetryingHttpClient>(std::move(real), retry_config);
}

} // namespace gcal
} // namespace duckdb
