#include "calendar/transport/retrying_http_client.hpp"

#include <chrono>
#include <random>
#include <thread>

namespace duckdb {
namespace gcal {

RetryingHttpClient::RetryingHttpClient(std::unique_ptr<IHttpClient> inner, RetryConfig config)
    : inner(std::move(inner)), config(config) {
}

bool RetryingHttpClient::IsRetryable(const HttpResponse &response) {
	int s = response.statusCode;
	if (s == 429) {
		return true;
	}
	if (s >= 500 && s <= 599) {
		return true;
	}
	if (s == 403) {
		const auto &b = response.body;
		if (b.find("rateLimitExceeded") != std::string::npos ||
		    b.find("userRateLimitExceeded") != std::string::npos) {
			return true;
		}
	}
	return false;
}

void RetryingHttpClient::SleepBackoff(int attempt) {
	if (config.zero_sleep) {
		return;
	}
	// Exponential backoff with full jitter: delay in [0, base * 2^attempt].
	long long base = (long long)config.base_delay_ms * (1LL << attempt);
	static thread_local std::mt19937 rng(std::random_device {}());
	std::uniform_int_distribution<long long> dist(0, base);
	long long delay = dist(rng);
	std::this_thread::sleep_for(std::chrono::milliseconds(delay));
}

HttpResponse RetryingHttpClient::Execute(const HttpRequest &request) {
	HttpResponse response;
	for (int attempt = 0; attempt < config.max_attempts; attempt++) {
		response = inner->Execute(request);
		if (!IsRetryable(response) || attempt + 1 == config.max_attempts) {
			return response;
		}
		SleepBackoff(attempt);
	}
	return response;
}
} // namespace gcal
} // namespace duckdb
