#pragma once

#include <memory>

#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

struct RetryConfig {
	int max_attempts = 5;
	int base_delay_ms = 1000;
	bool zero_sleep = false; // tests set this so the suite stays fast
};

class RetryingHttpClient : public IHttpClient {
public:
	RetryingHttpClient(std::unique_ptr<IHttpClient> inner, RetryConfig config = RetryConfig());
	HttpResponse Execute(const HttpRequest &request) override;
	static bool IsRetryable(const HttpResponse &response);

private:
	std::unique_ptr<IHttpClient> inner;
	RetryConfig config;
	void SleepBackoff(int attempt);
};
} // namespace gcal
} // namespace duckdb
