#include "storage/calendar_delete.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_transaction.hpp"
#include "storage/event_mapping.hpp"

#include "calendar/client.hpp"

namespace duckdb {

CalendarDelete::CalendarDelete(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
                               idx_t row_id_index, idx_t estimated_cardinality, bool return_chunk)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table), row_id_index(row_id_index), return_chunk(return_chunk) {
}

class CalendarDeleteGlobalSinkState : public GlobalSinkState {
public:
	gcal::GoogleCalendarClient *client = nullptr;
	string calendar_id;
	vector<string> names; // schema column index -> column name (only needed for RETURNING)
	idx_t delete_count = 0;
	vector<vector<Value>> returned; // RETURNING: one full-schema row per deleted event
};

unique_ptr<GlobalSinkState> CalendarDelete::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<CalendarDeleteGlobalSinkState>();
	auto &transaction = CalendarTransaction::Get(context, table.ParentCatalog());
	state->client = &transaction.GetClient(context);
	state->calendar_id = table.GetCalendarId();
	if (return_chunk) {
		for (auto &col : table.GetColumns().Logical()) {
			state->names.push_back(col.Name());
		}
	}
	return std::move(state);
}

SinkResultType CalendarDelete::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CalendarDeleteGlobalSinkState>();
	chunk.Flatten();
	for (idx_t row = 0; row < chunk.size(); row++) {
		auto id_val = chunk.GetValue(row_id_index, row);
		if (id_val.IsNull()) {
			continue;
		}
		string id = id_val.ToString();
		// RETURNING must report the deleted rows; the child only carries the row id, so fetch the
		// event body before removing it.
		if (return_chunk) {
			auto event = gstate.client->Events(gstate.calendar_id).Get(id);
			gstate.returned.push_back(gcal_map::EventToRow(event, gstate.names, gstate.calendar_id));
		}
		gstate.client->Events(gstate.calendar_id).Delete(id);
		gstate.delete_count++;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

class CalendarDeleteSourceState : public GlobalSourceState {
public:
	bool finished = false;
	idx_t offset = 0; // RETURNING: cursor into the collected rows
};

unique_ptr<GlobalSourceState> CalendarDelete::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CalendarDeleteSourceState>();
}

SourceResultType CalendarDelete::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CalendarDeleteSourceState>();
	auto &sink = sink_state->Cast<CalendarDeleteGlobalSinkState>();
	if (return_chunk) {
		gcal_map::EmitReturnedRows(chunk, sink.returned, state.offset);
		return state.offset >= sink.returned.size() ? SourceResultType::FINISHED
		                                             : SourceResultType::HAVE_MORE_OUTPUT;
	}
	if (state.finished) {
		return SourceResultType::FINISHED;
	}
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(sink.delete_count)));
	state.finished = true;
	return SourceResultType::FINISHED;
}

} // namespace duckdb
