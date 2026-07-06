#pragma once

#include <stdexcept>
#include <string>

namespace duckdb {
namespace gcal {

class CalendarException : public std::runtime_error {
public:
	explicit CalendarException(const std::string &message) : std::runtime_error(message) {
	}
};

class CalendarApiException : public CalendarException {
public:
	CalendarApiException(int statusCode, const std::string &apiMessage)
	    : CalendarException("Google Calendar API error (" + std::to_string(statusCode) + "): " + apiMessage),
	      statusCode(statusCode), apiMessage(apiMessage) {
	}
	int GetStatusCode() const {
		return statusCode;
	}
	const std::string &GetApiMessage() const {
		return apiMessage;
	}

private:
	int statusCode;
	std::string apiMessage;
};

class CalendarParseException : public CalendarException {
public:
	explicit CalendarParseException(const std::string &message) : CalendarException(message) {
	}
};
} // namespace gcal
} // namespace duckdb
