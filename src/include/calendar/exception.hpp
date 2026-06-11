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

class CalendarNotFoundException : public CalendarException {
public:
	explicit CalendarNotFoundException(const std::string &identifier)
	    : CalendarException("Calendar resource not found: " + identifier), identifier(identifier) {
	}
	const std::string &GetIdentifier() const {
		return identifier;
	}

private:
	std::string identifier;
};
} // namespace gcal
} // namespace duckdb
