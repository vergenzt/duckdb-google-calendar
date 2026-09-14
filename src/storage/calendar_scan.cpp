#include "storage/calendar_scan.hpp"
#include "storage/calendar_transaction.hpp"
#include "storage/calendar_catalog.hpp"
#include "storage/event_mapping.hpp"
#include "storage/calendar_mapping.hpp"

#include "json.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"

#include "calendar/client.hpp"
#include "calendar/util/query.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;

namespace duckdb {

static constexpr int64_t ONE_DAY_MICROS = 86400000000LL;

// ---------- shared: calendar enumeration ----------

static vector<gcal::CalendarListEntry> FetchCalendarList(gcal::GoogleCalendarClient &client) {
	vector<gcal::CalendarListEntry> result;
	string page_token;
	do {
		auto response = client.CalendarList().List(page_token);
		for (auto &cal : response.items) {
			result.push_back(cal);
		}
		page_token = response.nextPageToken;
	} while (!page_token.empty());
	return result;
}

// ---------- events ----------

struct EventsScanBindData : public TableFunctionData {
	EventsScanBindData(Catalog &catalog, TableCatalogEntry &table, vector<string> names)
	    : catalog(catalog), table(table), names(std::move(names)) {
	}
	Catalog &catalog;
	TableCatalogEntry &table; // target for MERGE/UPDATE/DELETE binding (LogicalGet::GetTable)
	vector<string> names;
	// Filled by pushdown_complex_filter (best-effort hints; the residual filter guarantees exactness).
	string time_min;
	string time_max;
	bool has_lower = false;
	bool has_upper = false;
	// Narrows the fan-out. When set, these ids are scanned directly without enumerating calendarList,
	// so an explicitly named calendar works even if the credential is not subscribed to it.
	bool has_calendar_filter = false;
	vector<string> calendar_ids;
};

struct EventsScanGlobalState : public GlobalTableFunctionState {
	gcal::GoogleCalendarClient *client = nullptr;
	string base_query;
	vector<string> calendar_ids; // the fan-out, resolved at init
	idx_t calendar_index = 0;
	string next_page_token;
	json items = json::array();
	idx_t item_index = 0;
	bool finished = false;
	vector<column_t> column_ids;

	const string &CurrentCalendar() const {
		return calendar_ids[calendar_index];
	}

	idx_t MaxThreads() const override {
		return 1;
	}
};

unique_ptr<FunctionData> MakeEventsScanBindData(Catalog &catalog, TableCatalogEntry &table, vector<string> names) {
	return make_uniq<EventsScanBindData>(catalog, table, std::move(names));
}

// ---------- filter -> timeMin/timeMax + calendar_id extraction ----------

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

// Is `expr` a reference to the named event column on this scan?
static bool IsColumn(LogicalGet &get, Expression &expr, const char *column_name) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	auto &cref = expr.Cast<BoundColumnRefExpression>();
	auto &col_ids = get.GetColumnIds();
	if (cref.binding.column_index >= col_ids.size()) {
		return false;
	}
	auto base_idx = col_ids[cref.binding.column_index].GetPrimaryIndex();
	return base_idx < get.names.size() && get.names[base_idx] == column_name;
}

static bool IsStartColumn(LogicalGet &get, Expression &expr) {
	return IsColumn(get, expr, "start");
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

static bool TryFoldVarchar(ClientContext &context, Expression &expr, string &out) {
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
	out = folded.ToString();
	return true;
}

static void ApplyLowerBound(const Value &ts, EventsScanBindData &bind_data) {
	bind_data.time_min = FormatWithBuffer(ts, -ONE_DAY_MICROS);
	bind_data.has_lower = true;
}

static void ApplyUpperBound(const Value &ts, EventsScanBindData &bind_data) {
	bind_data.time_max = FormatWithBuffer(ts, ONE_DAY_MICROS);
	bind_data.has_upper = true;
}

// Extract a timeMin/timeMax hint from a residual filter expression on `start`. Handles AND
// conjunctions (recursing), BETWEEN (the form `start >= A AND start < B` is rewritten to by the
// optimizer), and the binary comparison operators. The extracted bounds are only a best-effort
// API-side narrowing hint; the residual filter is always retained, so exactness is guaranteed.
static void ExtractTimeBound(ClientContext &context, LogicalGet &get, Expression &expr,
                             EventsScanBindData &bind_data) {
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

// Does `expr` restrict calendar_id to a finite set of literals? Sound in both directions: AND
// intersects (any conjunct that constrains narrows the whole), OR unions but only when *every* arm
// constrains, and anything else declines. `IN (...)` is handled both as COMPARE_IN and as the
// OR-of-equalities the optimizer often rewrites it to.
static bool TryExtractCalendarIds(ClientContext &context, LogicalGet &get, Expression &expr, vector<string> &out) {
	auto t = expr.GetExpressionType();

	if (t == ExpressionType::CONJUNCTION_AND) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		bool found = false;
		for (auto &child : conj.children) {
			vector<string> child_ids;
			if (!TryExtractCalendarIds(context, get, *child, child_ids)) {
				continue;
			}
			if (!found) {
				out = std::move(child_ids);
				found = true;
				continue;
			}
			std::unordered_set<string> keep(child_ids.begin(), child_ids.end());
			vector<string> intersected;
			for (auto &id : out) {
				if (keep.count(id)) {
					intersected.push_back(id);
				}
			}
			out = std::move(intersected);
		}
		return found;
	}

	if (t == ExpressionType::CONJUNCTION_OR) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		vector<string> unioned;
		for (auto &child : conj.children) {
			vector<string> child_ids;
			if (!TryExtractCalendarIds(context, get, *child, child_ids)) {
				// One unconstrained arm means the OR admits any calendar.
				return false;
			}
			unioned.insert(unioned.end(), child_ids.begin(), child_ids.end());
		}
		out = std::move(unioned);
		return true;
	}

	if (t == ExpressionType::COMPARE_EQUAL) {
		auto &cmp = expr.Cast<BoundComparisonExpression>();
		Expression *value_expr = nullptr;
		if (IsColumn(get, *cmp.left, "calendar_id")) {
			value_expr = cmp.right.get();
		} else if (IsColumn(get, *cmp.right, "calendar_id")) {
			value_expr = cmp.left.get();
		} else {
			return false;
		}
		string id;
		if (!TryFoldVarchar(context, *value_expr, id)) {
			return false;
		}
		out.push_back(id);
		return true;
	}

	if (t == ExpressionType::COMPARE_IN) {
		auto &op = expr.Cast<BoundOperatorExpression>();
		if (op.children.size() < 2 || !IsColumn(get, *op.children[0], "calendar_id")) {
			return false;
		}
		vector<string> ids;
		for (idx_t i = 1; i < op.children.size(); i++) {
			string id;
			if (!TryFoldVarchar(context, *op.children[i], id)) {
				return false;
			}
			ids.push_back(id);
		}
		out = std::move(ids);
		return true;
	}

	return false;
}

static void EventsComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                                vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<EventsScanBindData>();
	for (auto &expr : filters) {
		ExtractTimeBound(context, get, *expr, bind_data);
	}
	// Top-level filters are ANDed, so each one that constrains calendar_id narrows the fan-out further.
	for (auto &expr : filters) {
		vector<string> ids;
		if (!TryExtractCalendarIds(context, get, *expr, ids)) {
			continue;
		}
		if (!bind_data.has_calendar_filter) {
			bind_data.calendar_ids = std::move(ids);
			bind_data.has_calendar_filter = true;
			continue;
		}
		std::unordered_set<string> keep(ids.begin(), ids.end());
		vector<string> intersected;
		for (auto &id : bind_data.calendar_ids) {
			if (keep.count(id)) {
				intersected.push_back(id);
			}
		}
		bind_data.calendar_ids = std::move(intersected);
	}
	// Retain ALL expressions: DuckDB keeps a LogicalFilter so the exact predicate is re-applied.
}

// ---------- fetch + json -> row mapping ----------

// Fetches the next page of the calendar currently being scanned. An empty next_page_token means this
// is that calendar's first page.
static void FetchPage(EventsScanGlobalState &gstate, const EventsScanBindData &bind_data) {
	string query = gstate.base_query;
	if (!gstate.next_page_token.empty()) {
		query += "&pageToken=" + gcal::UrlEncode(gstate.next_page_token);
	}
	json response;
	try {
		response = gstate.client->Events(gstate.CurrentCalendar()).List(query);
	} catch (const std::exception &e) {
		throw IOException("google_calendar: events.list failed for calendar \"%s\": %s", gstate.CurrentCalendar(),
		                  e.what());
	}
	gstate.next_page_token = response.value("nextPageToken", string());
	if (response.contains("items") && response["items"].is_array()) {
		gstate.items = response["items"];
	} else {
		gstate.items = json::array();
	}
	gstate.item_index = 0;
}

// The calendars this scan will page through, most specific source first:
//   1. the pushed-down `calendar_id` predicate, if the query carried one;
//   2. else the catalog's `default_calendars` scope, if ATTACH set one;
//   3. else every non-deleted entry of calendarList.
// Only (3) costs an API call, so an attachment that sets default_calendars never enumerates at all.
static vector<string> ResolveFanOut(const EventsScanBindData &bind_data, gcal::GoogleCalendarClient &client) {
	if (bind_data.has_calendar_filter) {
		return bind_data.calendar_ids;
	}
	auto &catalog = bind_data.catalog.Cast<CalendarCatalog>();
	if (catalog.has_default_calendars) {
		return catalog.default_calendars;
	}
	vector<string> ids;
	for (auto &cal : FetchCalendarList(client)) {
		// Deleted entries linger in calendarList but events.list on them 404s.
		if (!cal.deleted) {
			ids.push_back(cal.id);
		}
	}
	return ids;
}

static unique_ptr<GlobalTableFunctionState> EventsScanInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<EventsScanBindData>();

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
	auto state = make_uniq<EventsScanGlobalState>();
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

	state->calendar_ids = ResolveFanOut(bind_data, *state->client);
	if (state->calendar_ids.empty()) {
		state->finished = true;
		return std::move(state);
	}
	FetchPage(*state, bind_data);
	return std::move(state);
}

static void EventsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<EventsScanBindData>();
	auto &gstate = data_p.global_state->Cast<EventsScanGlobalState>();

	idx_t out_idx = 0;
	while (out_idx < STANDARD_VECTOR_SIZE && !gstate.finished) {
		if (gstate.item_index >= gstate.items.size()) {
			if (!gstate.next_page_token.empty()) {
				FetchPage(gstate, bind_data); // next page of the current calendar
				continue;
			}
			if (gstate.calendar_index + 1 >= gstate.calendar_ids.size()) {
				gstate.finished = true;
				break;
			}
			gstate.calendar_index++; // fan out to the next calendar
			gstate.next_page_token.clear();
			FetchPage(gstate, bind_data);
			continue;
		}
		const auto &event = gstate.items[gstate.item_index++];
		auto &calendar_id = gstate.CurrentCalendar();
		for (idx_t col = 0; col < output.ColumnCount(); col++) {
			column_t cid = gstate.column_ids[col];
			if (cid == COLUMN_IDENTIFIER_ROW_ID) {
				// Row identity spans both halves: the event id alone does not say which calendar to
				// issue a subsequent UPDATE/DELETE against.
				auto event_id = gcal_map::JsonString(event, "id");
				output.SetValue(col, out_idx,
				                event_id.IsNull()
				                    ? Value(LogicalType::VARCHAR)
				                    : Value(gcal_map::PackRowId(calendar_id, event_id.ToString())));
			} else if (bind_data.names[cid] == "calendar_id") {
				output.SetValue(col, out_idx, Value(calendar_id));
			} else {
				output.SetValue(col, out_idx, gcal_map::ExtractField(event, bind_data.names[cid]));
			}
		}
		out_idx++;
	}
	output.SetCardinality(out_idx);
}

// Lets the binder resolve the scan's target table (LogicalGet::GetTable), which MERGE INTO and
// unqualified UPDATE/DELETE require — without it the binder throws "Can only merge into base tables!".
static BindInfo EventsScanGetBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	return BindInfo(bind_data_p->Cast<EventsScanBindData>().table);
}

TableFunction GetEventsScanFunction() {
	TableFunction function("google_calendar_events_scan", {}, EventsScan, nullptr, EventsScanInitGlobal);
	function.pushdown_complex_filter = EventsComplexFilter;
	function.projection_pushdown = true;
	function.get_bind_info = EventsScanGetBindInfo;
	return function;
}

// ---------- calendars ----------

struct CalendarsScanBindData : public TableFunctionData {
	CalendarsScanBindData(Catalog &catalog, TableCatalogEntry &table, vector<string> names)
	    : catalog(catalog), table(table), names(std::move(names)) {
	}
	Catalog &catalog;
	TableCatalogEntry &table;
	vector<string> names;
};

struct CalendarsScanGlobalState : public GlobalTableFunctionState {
	vector<gcal::CalendarListEntry> calendars;
	idx_t index = 0;
	vector<column_t> column_ids;

	idx_t MaxThreads() const override {
		return 1;
	}
};

unique_ptr<FunctionData> MakeCalendarsScanBindData(Catalog &catalog, TableCatalogEntry &table, vector<string> names) {
	return make_uniq<CalendarsScanBindData>(catalog, table, std::move(names));
}

static unique_ptr<GlobalTableFunctionState> CalendarsScanInitGlobal(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<CalendarsScanBindData>();
	auto state = make_uniq<CalendarsScanGlobalState>();
	state->column_ids = input.column_ids;
	auto &transaction = CalendarTransaction::Get(context, bind_data.catalog);
	// calendarList is small and unbounded scans of it are cheap, so unlike events there is no
	// mandatory filter here.
	state->calendars = FetchCalendarList(transaction.GetClient(context));
	return std::move(state);
}

static void CalendarsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<CalendarsScanBindData>();
	auto &gstate = data_p.global_state->Cast<CalendarsScanGlobalState>();

	idx_t out_idx = 0;
	while (out_idx < STANDARD_VECTOR_SIZE && gstate.index < gstate.calendars.size()) {
		auto &cal = gstate.calendars[gstate.index++];
		for (idx_t col = 0; col < output.ColumnCount(); col++) {
			column_t cid = gstate.column_ids[col];
			if (cid == COLUMN_IDENTIFIER_ROW_ID) {
				output.SetValue(col, out_idx, Value(cal.id));
			} else {
				output.SetValue(col, out_idx, gcal_map::ExtractCalendarField(cal, bind_data.names[cid]));
			}
		}
		out_idx++;
	}
	output.SetCardinality(out_idx);
}

static BindInfo CalendarsScanGetBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	return BindInfo(bind_data_p->Cast<CalendarsScanBindData>().table);
}

TableFunction GetCalendarsScanFunction() {
	TableFunction function("google_calendar_calendars_scan", {}, CalendarsScan, nullptr, CalendarsScanInitGlobal);
	function.projection_pushdown = true;
	function.get_bind_info = CalendarsScanGetBindInfo;
	return function;
}

} // namespace duckdb
