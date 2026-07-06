#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/types/value.hpp"

#include "calendar/util/options.hpp"

std::string duckdb::gcal::GetStringOption(const case_insensitive_map_t<Value> &options, const std::string &name,
                                          const std::string &default_value) {
	const auto it = options.find(name);
	if (it == options.end()) {
		return default_value;
	}
	std::string err;
	Value val;
	if (!it->second.DefaultTryCastAs(LogicalType::VARCHAR, val, &err)) {
		throw BinderException(name + " option must be VARCHAR");
	}
	if (val.IsNull()) {
		throw BinderException(name + " option must not be NULL");
	}
	return StringValue::Get(val);
}
