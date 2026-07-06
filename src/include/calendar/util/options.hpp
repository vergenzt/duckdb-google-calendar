#pragma once

#include <string>

#include "duckdb/common/types/value.hpp"

namespace duckdb {
namespace gcal {

std::string GetStringOption(const case_insensitive_map_t<Value> &options, const std::string &name,
                            const std::string &default_value = "");

} // namespace gcal
} // namespace duckdb
