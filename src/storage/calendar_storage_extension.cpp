#include "storage/calendar_storage_extension.hpp"
#include "storage/calendar_catalog.hpp"
#include "storage/calendar_transaction_manager.hpp"

#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

static unique_ptr<Catalog> CalendarAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AttachOptions &options) {
	string secret_name;
	auto secret_entry = info.options.find("secret");
	if (secret_entry != info.options.end()) {
		secret_name = StringValue::Get(secret_entry->second);
	}

	auto catalog = make_uniq<CalendarCatalog>(db, info.path, secret_name);

	// Optional calendar_aliases: MAP(VARCHAR, VARCHAR) of calendar ID -> mounted table name.
	auto alias_it = info.options.find("calendar_aliases");
	if (alias_it != info.options.end()) {
		auto &v = alias_it->second;
		if (v.type().id() != LogicalTypeId::MAP || MapType::KeyType(v.type()).id() != LogicalTypeId::VARCHAR ||
		    MapType::ValueType(v.type()).id() != LogicalTypeId::VARCHAR) {
			throw BinderException("calendar_aliases must be a MAP(VARCHAR, VARCHAR) of calendar ID -> table name");
		}
		for (auto &entry : ListValue::GetChildren(v)) {
			auto &kv = StructValue::GetChildren(entry);
			catalog->calendar_aliases[StringValue::Get(kv[0])] = StringValue::Get(kv[1]);
		}
	}

	// Optional default window that lets an unbounded scan (and thus MERGE / unqualified
	// UPDATE/DELETE) fall back to a time range instead of erroring. Configure with any two of
	// {default_window_start, default_window_end, default_window_length}, or length alone:
	//   default_window_length alone            -> dynamic [now - length/2, now + length/2] (per query)
	//   default_window_start + default_window_end    -> static [start, end]
	//   default_window_start + default_window_length -> static [start, start + length]
	//   default_window_end   + default_window_length -> static [end - length, end]
	// default_window_length accepts ISO ('P90D') or friendly ('3 months') intervals; start/end are
	// TIMESTAMP WITH TIME ZONE.
	auto s_it = info.options.find("default_window_start");
	auto e_it = info.options.find("default_window_end");
	auto w_it = info.options.find("default_window_length");
	bool has_s = s_it != info.options.end();
	bool has_e = e_it != info.options.end();
	bool has_w = w_it != info.options.end();
	if (has_s || has_e || has_w) {
		auto to_ts = [](const Value &v, const char *name) {
			Value out;
			string err;
			if (!v.DefaultTryCastAs(LogicalType::TIMESTAMP_TZ, out, &err)) {
				throw BinderException(string(name) + " must be a TIMESTAMP WITH TIME ZONE: " + err);
			}
			return out.GetValue<timestamp_tz_t>();
		};
		auto to_iv = [](const Value &v) {
			Value out;
			string err;
			if (!v.DefaultTryCastAs(LogicalType::INTERVAL, out, &err)) {
				throw BinderException("default_window_length must be an INTERVAL (e.g. 'P90D', '3 months'): " + err);
			}
			return IntervalValue::Get(out);
		};
		if (has_s && has_e && has_w) {
			throw BinderException(
			    "specify at most two of default_window_start / default_window_end / default_window_length");
		} else if (has_w && !has_s && !has_e) {
			catalog->default_window_length = to_iv(w_it->second);
			catalog->default_is_dynamic = true;
		} else if (has_s && has_e) {
			catalog->default_window_start = to_ts(s_it->second, "default_window_start");
			catalog->default_window_end = to_ts(e_it->second, "default_window_end");
		} else if (has_s && has_w) {
			auto s = to_ts(s_it->second, "default_window_start");
			catalog->default_window_start = s;
			catalog->default_window_end = timestamp_tz_t(Interval::Add(timestamp_t(s.value), to_iv(w_it->second)));
		} else if (has_e && has_w) {
			auto e = to_ts(e_it->second, "default_window_end");
			auto w = to_iv(w_it->second);
			interval_t neg {-w.months, -w.days, -w.micros};
			catalog->default_window_start = timestamp_tz_t(Interval::Add(timestamp_t(e.value), neg));
			catalog->default_window_end = e;
		} else {
			throw BinderException("default_window_start / default_window_end must be paired with each other "
			                      "or with default_window_length");
		}
		catalog->has_default_window = true;
	}

	catalog->LoadCatalog(context);
	return std::move(catalog);
}

static unique_ptr<TransactionManager> CalendarCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                       AttachedDatabase &db, Catalog &catalog) {
	auto &calendar_catalog = catalog.Cast<CalendarCatalog>();
	return make_uniq<CalendarTransactionManager>(db, calendar_catalog);
}

CalendarStorageExtension::CalendarStorageExtension() {
	attach = CalendarAttach;
	create_transaction_manager = CalendarCreateTransactionManager;
}

} // namespace duckdb
