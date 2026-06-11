#include "storage/calendar_update.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_transaction.hpp"
#include "storage/event_mapping.hpp"

#include "calendar/client.hpp"

#include "json.hpp"

using json = nlohmann::json;

namespace duckdb {

CalendarUpdate::CalendarUpdate(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
                               vector<PhysicalIndex> columns, vector<idx_t> value_indices,
                               idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table), columns(std::move(columns)), value_indices(std::move(value_indices)) {
}

class CalendarUpdateGlobalSinkState : public GlobalSinkState {
public:
	gcal::GoogleCalendarClient *client = nullptr;
	string calendar_id;
	idx_t update_count = 0;
};

unique_ptr<GlobalSinkState> CalendarUpdate::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<CalendarUpdateGlobalSinkState>();
	auto &transaction = CalendarTransaction::Get(context, table.ParentCatalog());
	state->client = &transaction.GetClient(context);
	state->calendar_id = table.GetCalendarId();
	return std::move(state);
}

SinkResultType CalendarUpdate::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CalendarUpdateGlobalSinkState>();
	chunk.Flatten();
	// Child chunk layout (core PhysicalUpdate): SET values at value_indices[c], rowid at the last column.
	idx_t row_id_index = chunk.ColumnCount() - 1;
	for (idx_t row = 0; row < chunk.size(); row++) {
		auto id_val = chunk.GetValue(row_id_index, row);
		if (id_val.IsNull()) {
			continue;
		}
		string id = id_val.ToString();
		json event = gstate.client->Events(gstate.calendar_id).Get(id);

		bool all_day = gcal_map::ExistingAllDay(event);
		for (idx_t c = 0; c < columns.size(); c++) {
			if (columns[c].index == 10) {
				auto v = chunk.GetValue(value_indices[c], row);
				all_day = !v.IsNull() && BooleanValue::Get(v);
			}
		}
		for (idx_t c = 0; c < columns.size(); c++) {
			gcal_map::ApplySet(event, columns[c].index, chunk.GetValue(value_indices[c], row), all_day);
		}
		gstate.client->Events(gstate.calendar_id).Update(id, event);
		gstate.update_count++;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

class CalendarUpdateSourceState : public GlobalSourceState {
public:
	bool finished = false;
};

unique_ptr<GlobalSourceState> CalendarUpdate::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CalendarUpdateSourceState>();
}

SourceResultType CalendarUpdate::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CalendarUpdateSourceState>();
	if (state.finished) {
		return SourceResultType::FINISHED;
	}
	auto &sink = sink_state->Cast<CalendarUpdateGlobalSinkState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(sink.update_count)));
	state.finished = true;
	return SourceResultType::FINISHED;
}

} // namespace duckdb
