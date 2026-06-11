#include "calendar/util/version.hpp"

std::string duckdb::getVersion() {
#ifdef EXT_VERSION_GOOGLE_CALENDAR
	return EXT_VERSION_GOOGLE_CALENDAR;
#else
	return "";
#endif
}
