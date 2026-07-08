#include "storage/calendar_scan.hpp"
#include "storage/calendar_transaction.hpp"
#include "storage/calendar_catalog.hpp"

#include "json.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"

#include "calendar/client.hpp"
#include "calendar/util/query.hpp"

#include <algorithm>
#include <unordered_map>

using json = nlohmann::json;

namespace duckdb {

static constexpr int64_t ONE_DAY_MICROS = 86400000000LL;

struct CalendarScanBindData : public TableFunctionData {
	CalendarScanBindData(Catalog &catalog, TableCatalogEntry &table, string calendar_id, vector<string> names,
	                     vector<LogicalType> types)
	    : catalog(catalog), table(table), calendar_id(std::move(calendar_id)), names(std::move(names)),
	      types(std::move(types)) {
	}
	Catalog &catalog;
	TableCatalogEntry &table; // target for MERGE/UPDATE/DELETE binding (LogicalGet::GetTable)
	string calendar_id;
	vector<string> names;
	vector<LogicalType> types;
	// Filled by pushdown_complex_filter (best-effort hint; residual guarantees exactness).
	string time_min;
	string time_max;
	bool has_lower = false;
	bool has_upper = false;
};

struct CalendarScanGlobalState : public GlobalTableFunctionState {
	gcal::GoogleCalendarClient *client = nullptr;
	string base_query;
	string next_page_token;
	json items = json::array();
	idx_t item_index = 0;
	bool finished = false;
	vector<column_t> column_ids;

	idx_t MaxThreads() const override {
		return 1;
	}
};

unique_ptr<FunctionData> MakeCalendarScanBindData(Catalog &catalog, TableCatalogEntry &table, string calendar_id,
                                                  vector<string> names, vector<LogicalType> types) {
	return make_uniq<CalendarScanBindData>(catalog, table, std::move(calendar_id), std::move(names), std::move(types));
}

// ---------- filter -> timeMin/timeMax extraction ----------

static ExpressionType FlipComparison(ExpressionType t) {
	switch (t) {
	case ExpressionType::COMPARE_GREATERTHAN:
		return ExpressionType::COMPARE_LESSTHAN;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ExpressionType::COMPARE_LESSTHANOREQUALTO;
	case ExpressionType::COMPARE_LESSTHAN:
		return ExpressionType::COMPARE_GREATERTHAN;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	default:
		return t;
	}
}

static string FormatWithBuffer(const Value &val, int64_t buffer_micros) {
	auto tstz = val.GetValue<timestamp_tz_t>();
	timestamp_t ts(tstz.value + buffer_micros);
	string s = Timestamp::ToString(ts);
	std::replace(s.begin(), s.end(), ' ', 'T');
	return s + "Z";
}

// Is `expr` a reference to the `start` event column on this scan?
static bool IsStartColumn(LogicalGet &get, Expression &expr) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	auto &cref = expr.Cast<BoundColumnRefExpression>();
	auto &col_ids = get.GetColumnIds();
	if (cref.binding.column_index >= col_ids.size()) {
		return false;
	}
	auto base_idx = col_ids[cref.binding.column_index].GetPrimaryIndex();
	return base_idx < get.names.size() && get.names[base_idx] == "start";
}

// Fold a constant-foldable expression into a TIMESTAMP WITH TIME ZONE value. The bound literal
// usually arrives un-folded (e.g. `CAST('...' AS TIMESTAMP WITH TIME ZONE)`), so we evaluate it
// rather than requiring a bare VALUE_CONSTANT node. Returns false if the side references columns,
// is NULL, or cannot be cast to TIMESTAMP_TZ.
static bool TryFoldTimestamp(ClientContext &context, Expression &expr, Value &out) {
	if (!expr.IsFoldable()) {
		return false;
	}
	Value folded;
	try {
		folded = ExpressionExecutor::EvaluateScalar(context, expr);
	} catch (const std::exception &) {
		return false;
	}
	if (folded.IsNull()) {
		return false;
	}
	string err;
	return folded.DefaultTryCastAs(LogicalType::TIMESTAMP_TZ, out, &err);
}

static void ApplyLowerBound(const Value &ts, CalendarScanBindData &bind_data) {
	bind_data.time_min = FormatWithBuffer(ts, -ONE_DAY_MICROS);
	bind_data.has_lower = true;
}

static void ApplyUpperBound(const Value &ts, CalendarScanBindData &bind_data) {
	bind_data.time_max = FormatWithBuffer(ts, ONE_DAY_MICROS);
	bind_data.has_upper = true;
}

// Extract a timeMin/timeMax hint from a residual filter expression on `start`. Handles AND
// conjunctions (recursing), BETWEEN (the form `start >= A AND start < B` is rewritten to by the
// optimizer), and the binary comparison operators. The extracted bounds are only a best-effort
// API-side narrowing hint; the residual filter is always retained, so exactness is guaranteed.
static void ExtractTimeBound(ClientContext &context, LogicalGet &get, Expression &expr,
                             CalendarScanBindData &bind_data) {
	auto t = expr.GetExpressionType();

	// AND conjunction: recurse into each conjunct.
	if (t == ExpressionType::CONJUNCTION_AND) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		for (auto &child : conj.children) {
			ExtractTimeBound(context, get, *child, bind_data);
		}
		return;
	}

	// BETWEEN: `start BETWEEN lower AND upper` (what `start >= A AND start < B` is rewritten to).
	if (t == ExpressionType::COMPARE_BETWEEN) {
		auto &between = expr.Cast<BoundBetweenExpression>();
		if (!IsStartColumn(get, *between.input)) {
			return;
		}
		Value ts;
		if (TryFoldTimestamp(context, *between.lower, ts)) {
			ApplyLowerBound(ts, bind_data);
		}
		if (TryFoldTimestamp(context, *between.upper, ts)) {
			ApplyUpperBound(ts, bind_data);
		}
		return;
	}

	// Binary comparison: `start <op> value` or `value <op> start`.
	if (t != ExpressionType::COMPARE_GREATERTHAN && t != ExpressionType::COMPARE_GREATERTHANOREQUALTO &&
	    t != ExpressionType::COMPARE_LESSTHAN && t != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
	    t != ExpressionType::COMPARE_EQUAL) {
		return;
	}
	auto &cmp = expr.Cast<BoundComparisonExpression>();
	Expression *value_expr = nullptr;
	bool inverted = false;
	if (IsStartColumn(get, *cmp.left)) {
		value_expr = cmp.right.get();
	} else if (IsStartColumn(get, *cmp.right)) {
		value_expr = cmp.left.get();
		inverted = true;
	} else {
		return;
	}

	Value ts;
	if (!TryFoldTimestamp(context, *value_expr, ts)) {
		return;
	}

	auto op = inverted ? FlipComparison(t) : t;
	if (op == ExpressionType::COMPARE_GREATERTHAN || op == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	    op == ExpressionType::COMPARE_EQUAL) {
		ApplyLowerBound(ts, bind_data);
	}
	if (op == ExpressionType::COMPARE_LESSTHAN || op == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	    op == ExpressionType::COMPARE_EQUAL) {
		ApplyUpperBound(ts, bind_data);
	}
}

static void CalendarComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                                  vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<CalendarScanBindData>();
	for (auto &expr : filters) {
		ExtractTimeBound(context, get, *expr, bind_data);
	}
	// Retain ALL expressions: DuckDB keeps a LogicalFilter so the exact predicate is re-applied.
}

// ---------- fetch + json -> row mapping ----------

static void FetchPage(CalendarScanGlobalState &gstate, const CalendarScanBindData &bind_data) {
	string query = gstate.base_query;
	if (!gstate.next_page_token.empty()) {
		query += "&pageToken=" + gcal::UrlEncode(gstate.next_page_token);
	}
	auto response = gstate.client->Events(bind_data.calendar_id).List(query);
	gstate.next_page_token = response.value("nextPageToken", string());
	if (response.contains("items") && response["items"].is_array()) {
		gstate.items = response["items"];
	} else {
		gstate.items = json::array();
	}
	gstate.item_index = 0;
}

static Value JsonString(const json &event, const char *key) {
	if (!event.contains(key) || event[key].is_null()) {
		return Value(LogicalType::VARCHAR);
	}
	if (event[key].is_string()) {
		return Value(event[key].get<string>());
	}
	return Value(event[key].dump());
}

static Value JsonRaw(const json &event, const char *key) {
	if (!event.contains(key) || event[key].is_null()) {
		return Value(LogicalType::VARCHAR);
	}
	return Value(event[key].dump());
}

static Value JsonBool(const json &event, const char *key) {
	if (!event.contains(key) || !event[key].is_boolean()) {
		return Value(LogicalType::BOOLEAN);
	}
	return Value::BOOLEAN(event[key].get<bool>());
}

static Value JsonBigint(const json &event, const char *key) {
	if (!event.contains(key) || !event[key].is_number_integer()) {
		return Value(LogicalType::BIGINT);
	}
	return Value::BIGINT(event[key].get<int64_t>());
}

static bool IsAllDay(const json &event) {
	return event.contains("start") && event["start"].is_object() && event["start"].contains("date") &&
	       !event["start"].contains("dateTime");
}

static Value ParseEventTime(const json &event, const char *which) {
	if (!event.contains(which) || !event[which].is_object()) {
		return Value(LogicalType::TIMESTAMP_TZ);
	}
	const auto &node = event[which];
	string raw;
	if (node.contains("dateTime") && node["dateTime"].is_string()) {
		raw = node["dateTime"].get<string>();
	} else if (node.contains("date") && node["date"].is_string()) {
		raw = node["date"].get<string>();
	} else {
		return Value(LogicalType::TIMESTAMP_TZ);
	}
	Value out;
	string err;
	if (Value(raw).DefaultTryCastAs(LogicalType::TIMESTAMP_TZ, out, &err)) {
		return out;
	}
	return Value(LogicalType::TIMESTAMP_TZ);
}

static Value ExtractField(const json &event, const string &field) {
	// Plain-string passthrough columns -> Google JSON key.
	static const std::unordered_map<string, const char *> string_keys = {
	    {"id", "id"},
	    {"summary", "summary"},
	    {"description", "description"},
	    {"location", "location"},
	    {"status", "status"},
	    {"html_link", "htmlLink"},
	    {"created", "created"},
	    {"updated", "updated"},
	    {"color_id", "colorId"},
	    {"transparency", "transparency"},
	    {"visibility", "visibility"},
	    {"event_type", "eventType"},
	    {"recurring_event_id", "recurringEventId"},
	    {"ical_uid", "iCalUID"},
	    {"etag", "etag"},
	    {"kind", "kind"},
	    {"hangout_link", "hangoutLink"},
	};
	// Raw-JSON passthrough columns (objects/arrays) -> Google JSON key.
	static const std::unordered_map<string, const char *> raw_keys = {
	    {"attendees", "attendees"},
	    {"recurrence", "recurrence"},
	    {"reminders", "reminders"},
	    {"conference_data", "conferenceData"},
	    {"creator", "creator"},
	    {"organizer", "organizer"},
	    {"extended_properties", "extendedProperties"},
	    {"source", "source"},
	    {"attachments", "attachments"},
	    {"working_location_properties", "workingLocationProperties"},
	    {"out_of_office_properties", "outOfOfficeProperties"},
	    {"focus_time_properties", "focusTimeProperties"},
	};
	// Boolean columns -> Google JSON key.
	static const std::unordered_map<string, const char *> bool_keys = {
	    {"end_time_unspecified", "endTimeUnspecified"},
	    {"attendees_omitted", "attendeesOmitted"},
	    {"anyone_can_add_self", "anyoneCanAddSelf"},
	    {"guests_can_invite_others", "guestsCanInviteOthers"},
	    {"guests_can_modify", "guestsCanModify"},
	    {"guests_can_see_other_guests", "guestsCanSeeOtherGuests"},
	    {"private_copy", "privateCopy"},
	    {"locked", "locked"},
	};

	auto s = string_keys.find(field);
	if (s != string_keys.end()) {
		return JsonString(event, s->second);
	}
	auto r = raw_keys.find(field);
	if (r != raw_keys.end()) {
		return JsonRaw(event, r->second);
	}
	auto b = bool_keys.find(field);
	if (b != bool_keys.end()) {
		return JsonBool(event, b->second);
	}
	// Fields needing bespoke decoding.
	if (field == "start") {
		return ParseEventTime(event, "start");
	}
	if (field == "end") {
		return ParseEventTime(event, "end");
	}
	if (field == "original_start_time") {
		return ParseEventTime(event, "originalStartTime");
	}
	if (field == "all_day") {
		return Value::BOOLEAN(IsAllDay(event));
	}
	if (field == "sequence") {
		return JsonBigint(event, "sequence");
	}
	return Value();
}

static unique_ptr<GlobalTableFunctionState> CalendarScanInitGlobal(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<CalendarScanBindData>();

	// Effective bounds: the explicit filter bounds if present, else the catalog's rolling
	// default_window (which is what makes MERGE and unqualified UPDATE/DELETE reachable — their
	// target scan carries no `start` filter to bound the API call).
	bool has_lower = bind_data.has_lower;
	bool has_upper = bind_data.has_upper;
	string time_min = bind_data.time_min;
	string time_max = bind_data.time_max;
	if (!has_lower && !has_upper) {
		auto &cat = bind_data.catalog.Cast<CalendarCatalog>();
		if (!cat.has_default_window) {
			throw BinderException(
			    "google_calendar scan requires an explicit time bound on \"start\" (e.g. "
			    "WHERE start >= TIMESTAMPTZ '2026-06-01 00:00:00+00' AND start < TIMESTAMPTZ '2026-07-01 00:00:00+00'), "
			    "or ATTACH with default_window_length / default_window_start / default_window_end to enable a "
			    "default window");
		}
		timestamp_tz_t lo, hi;
		if (cat.default_is_dynamic) {
			// length/2 each side of "now". Halving in micros (months approximated) is fine — the
			// FormatWithBuffer ±1-day pad already fuzzes the edges.
			auto now = Timestamp::GetCurrentTimestamp();
			int64_t half = Interval::GetMicro(cat.default_window_length) / 2;
			lo = timestamp_tz_t(now.value - half);
			hi = timestamp_tz_t(now.value + half);
		} else {
			lo = cat.default_window_start;
			hi = cat.default_window_end;
		}
		time_min = FormatWithBuffer(Value::TIMESTAMPTZ(lo), -ONE_DAY_MICROS);
		time_max = FormatWithBuffer(Value::TIMESTAMPTZ(hi), ONE_DAY_MICROS);
		has_lower = has_upper = true;
	}
	auto state = make_uniq<CalendarScanGlobalState>();
	state->column_ids = input.column_ids;

	gcal::QueryBuilder qb;
	qb.Add("singleEvents", "true").Add("orderBy", "startTime").Add("maxResults", "2500");
	if (has_lower) {
		qb.Add("timeMin", time_min);
	}
	if (has_upper) {
		qb.Add("timeMax", time_max);
	}
	state->base_query = qb.Build();

	auto &transaction = CalendarTransaction::Get(context, bind_data.catalog);
	state->client = &transaction.GetClient(context);

	FetchPage(*state, bind_data);
	return std::move(state);
}

static void CalendarScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<CalendarScanBindData>();
	auto &gstate = data_p.global_state->Cast<CalendarScanGlobalState>();

	idx_t out_idx = 0;
	while (out_idx < STANDARD_VECTOR_SIZE && !gstate.finished) {
		if (gstate.item_index >= gstate.items.size()) {
			if (gstate.next_page_token.empty()) {
				gstate.finished = true;
				break;
			}
			FetchPage(gstate, bind_data);
			continue;
		}
		const auto &event = gstate.items[gstate.item_index++];
		for (idx_t col = 0; col < output.ColumnCount(); col++) {
			column_t cid = gstate.column_ids[col];
			if (cid == COLUMN_IDENTIFIER_ROW_ID) {
				output.SetValue(col, out_idx, JsonString(event, "id"));
			} else if (cid == CALENDAR_ID_VIRTUAL_COLUMN) {
				output.SetValue(col, out_idx, Value(bind_data.calendar_id));
			} else {
				output.SetValue(col, out_idx, ExtractField(event, bind_data.names[cid]));
			}
		}
		out_idx++;
	}
	output.SetCardinality(out_idx);
}

// Lets the binder resolve the scan's target table (LogicalGet::GetTable), which MERGE INTO and
// unqualified UPDATE/DELETE require — without it the binder throws "Can only merge into base tables!".
static BindInfo CalendarScanGetBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	return BindInfo(bind_data_p->Cast<CalendarScanBindData>().table);
}

TableFunction GetCalendarScanFunction() {
	TableFunction function("google_calendar_scan", {}, CalendarScan, nullptr, CalendarScanInitGlobal);
	function.pushdown_complex_filter = CalendarComplexFilter;
	function.projection_pushdown = true;
	function.get_bind_info = CalendarScanGetBindInfo;
	return function;
}

} // namespace duckdb
