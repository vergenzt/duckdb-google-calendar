#include "storage/calendar_delete.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_transaction.hpp"

#include "calendar/client.hpp"

namespace duckdb {

CalendarDelete::CalendarDelete(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
                               idx_t row_id_index, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table), row_id_index(row_id_index) {
}

class CalendarDeleteGlobalSinkState : public GlobalSinkState {
public:
	gcal::GoogleCalendarClient *client = nullptr;
	string calendar_id;
	idx_t delete_count = 0;
};

unique_ptr<GlobalSinkState> CalendarDelete::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<CalendarDeleteGlobalSinkState>();
	auto &transaction = CalendarTransaction::Get(context, table.ParentCatalog());
	state->client = &transaction.GetClient(context);
	state->calendar_id = table.GetCalendarId();
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
		gstate.client->Events(gstate.calendar_id).Delete(id_val.ToString());
		gstate.delete_count++;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

class CalendarDeleteSourceState : public GlobalSourceState {
public:
	bool finished = false;
};

unique_ptr<GlobalSourceState> CalendarDelete::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CalendarDeleteSourceState>();
}

SourceResultType CalendarDelete::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CalendarDeleteSourceState>();
	if (state.finished) {
		return SourceResultType::FINISHED;
	}
	auto &sink = sink_state->Cast<CalendarDeleteGlobalSinkState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(sink.delete_count)));
	state.finished = true;
	return SourceResultType::FINISHED;
}

} // namespace duckdb
