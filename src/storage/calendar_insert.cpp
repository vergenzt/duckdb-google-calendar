#include "storage/calendar_insert.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_transaction.hpp"
#include "storage/event_mapping.hpp"

#include "calendar/client.hpp"

namespace duckdb {

CalendarInsert::CalendarInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
                               idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table) {
}

class CalendarInsertGlobalSinkState : public GlobalSinkState {
public:
	gcal::GoogleCalendarClient *client = nullptr;
	string calendar_id;
	idx_t insert_count = 0;
};

unique_ptr<GlobalSinkState> CalendarInsert::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<CalendarInsertGlobalSinkState>();
	auto &transaction = CalendarTransaction::Get(context, table.ParentCatalog());
	state->client = &transaction.GetClient(context);
	state->calendar_id = table.GetCalendarId();
	return std::move(state);
}

SinkResultType CalendarInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CalendarInsertGlobalSinkState>();
	chunk.Flatten();
	for (idx_t row = 0; row < chunk.size(); row++) {
		auto event = gcal_map::RowToEvent(chunk, row);
		gstate.client->Events(gstate.calendar_id).Insert(event);
		gstate.insert_count++;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

class CalendarInsertSourceState : public GlobalSourceState {
public:
	bool finished = false;
};

unique_ptr<GlobalSourceState> CalendarInsert::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CalendarInsertSourceState>();
}

SourceResultType CalendarInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CalendarInsertSourceState>();
	if (state.finished) {
		return SourceResultType::FINISHED;
	}
	auto &sink = sink_state->Cast<CalendarInsertGlobalSinkState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(sink.insert_count)));
	state.finished = true;
	return SourceResultType::FINISHED;
}

} // namespace duckdb
