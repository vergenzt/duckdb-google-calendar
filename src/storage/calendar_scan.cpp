#include "storage/calendar_scan.hpp"
#include "storage/calendar_transaction.hpp"

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

using json = nlohmann::json;

namespace duckdb {

static constexpr int64_t ONE_DAY_MICROS = 86400000000LL;

struct CalendarScanBindData : public TableFunctionData {
	CalendarScanBindData(Catalog &catalog, string calendar_id, vector<string> names, vector<LogicalType> types)
	    : catalog(catalog), calendar_id(std::move(calendar_id)), names(std::move(names)), types(std::move(types)) {
	}
	Catalog &catalog;
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

unique_ptr<FunctionData> MakeCalendarScanBindData(Catalog &catalog, string calendar_id, vector<string> names,
                                                  vector<LogicalType> types) {
	return make_uniq<CalendarScanBindData>(catalog, std::move(calendar_id), std::move(names), std::move(types));
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
	if (field == "id") {
		return JsonString(event, "id");
	}
	if (field == "summary") {
		return JsonString(event, "summary");
	}
	if (field == "description") {
		return JsonString(event, "description");
	}
	if (field == "location") {
		return JsonString(event, "location");
	}
	if (field == "status") {
		return JsonString(event, "status");
	}
	if (field == "html_link") {
		return JsonString(event, "htmlLink");
	}
	if (field == "created") {
		return JsonString(event, "created");
	}
	if (field == "updated") {
		return JsonString(event, "updated");
	}
	if (field == "start") {
		return ParseEventTime(event, "start");
	}
	if (field == "end") {
		return ParseEventTime(event, "end");
	}
	if (field == "all_day") {
		return Value::BOOLEAN(IsAllDay(event));
	}
	if (field == "attendees") {
		return JsonRaw(event, "attendees");
	}
	if (field == "recurrence") {
		return JsonRaw(event, "recurrence");
	}
	if (field == "reminders") {
		return JsonRaw(event, "reminders");
	}
	if (field == "conference_data") {
		return JsonRaw(event, "conferenceData");
	}
	return Value();
}

static unique_ptr<GlobalTableFunctionState> CalendarScanInitGlobal(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<CalendarScanBindData>();
	if (!bind_data.has_lower && !bind_data.has_upper) {
		throw BinderException(
		    "google_calendar scan requires an explicit time bound on \"start\" (e.g. "
		    "WHERE start >= TIMESTAMPTZ '2026-06-01 00:00:00+00' AND start < TIMESTAMPTZ '2026-07-01 00:00:00+00')");
	}
	auto state = make_uniq<CalendarScanGlobalState>();
	state->column_ids = input.column_ids;

	gcal::QueryBuilder qb;
	qb.Add("singleEvents", "true").Add("orderBy", "startTime").Add("maxResults", "2500");
	if (bind_data.has_lower) {
		qb.Add("timeMin", bind_data.time_min);
	}
	if (bind_data.has_upper) {
		qb.Add("timeMax", bind_data.time_max);
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
			} else {
				output.SetValue(col, out_idx, ExtractField(event, bind_data.names[cid]));
			}
		}
		out_idx++;
	}
	output.SetCardinality(out_idx);
}

TableFunction GetCalendarScanFunction() {
	TableFunction function("google_calendar_scan", {}, CalendarScan, nullptr, CalendarScanInitGlobal);
	function.pushdown_complex_filter = CalendarComplexFilter;
	function.projection_pushdown = true;
	return function;
}

} // namespace duckdb
