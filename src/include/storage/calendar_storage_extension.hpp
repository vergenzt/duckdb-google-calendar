#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

class CalendarStorageExtension : public StorageExtension {
public:
	CalendarStorageExtension();
};

} // namespace duckdb
