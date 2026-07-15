#include "storage/calendar_insert.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_transaction.hpp"
#include "storage/event_mapping.hpp"

#include "calendar/client.hpp"
#include "calendar/exception.hpp"

namespace duckdb {

CalendarInsert::CalendarInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
                               idx_t estimated_cardinality, bool return_chunk)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table), return_chunk(return_chunk) {
}

class CalendarInsertGlobalSinkState : public GlobalSinkState {
public:
	gcal::GoogleCalendarClient *client = nullptr;
	string calendar_id;
	vector<string> names; // chunk column index -> schema column name
	idx_t insert_count = 0;
	vector<vector<Value>> returned; // RETURNING: one full-schema row per inserted event
};

unique_ptr<GlobalSinkState> CalendarInsert::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<CalendarInsertGlobalSinkState>();
	auto &transaction = CalendarTransaction::Get(context, table.ParentCatalog());
	state->client = &transaction.GetClient(context);
	state->calendar_id = table.GetCalendarId();
	for (auto &col : table.GetColumns().Logical()) {
		state->names.push_back(col.Name());
	}
	return std::move(state);
}

SinkResultType CalendarInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CalendarInsertGlobalSinkState>();
	chunk.Flatten();
	for (idx_t row = 0; row < chunk.size(); row++) {
		auto event = gcal_map::RowToEvent(chunk, row, gstate.names);
		nlohmann::json created;
		try {
			created = gstate.client->Events(gstate.calendar_id).Insert(event);
		} catch (const gcal::CalendarApiException &e) {
			// 409 = the id already exists server-side: either a replica living outside the scan window,
			// or a deleted event whose id Google reserves for months (events.insert can't reuse it). The
			// MERGE saw "not matched" only because events.list hides both cases. events.update (PUT)
			// overwrites/resurrects in place, turning this insert into an upsert against the reserved id.
			if (e.GetStatusCode() == 409 && event.contains("id")) {
				created = gstate.client->Events(gstate.calendar_id).Update(event["id"].get<string>(), event);
			} else {
				auto id = event.contains("id") ? event["id"].get<string>() : string("(server-assigned)");
				throw IOException("google_calendar: events.insert failed for event_id=%s on calendar %s: %s\n"
				                  "request body: %s",
				                  id, gstate.calendar_id, e.what(), event.dump());
			}
		} catch (const std::exception &e) {
			// Surface which event failed: the client-supplied id and full request body are otherwise
			// lost, making a non-API failure impossible to trace back to a source event.
			auto id = event.contains("id") ? event["id"].get<string>() : string("(server-assigned)");
			throw IOException("google_calendar: events.insert failed for event_id=%s on calendar %s: %s\n"
			                  "request body: %s",
			                  id, gstate.calendar_id, e.what(), event.dump());
		}
		gstate.insert_count++;
		if (return_chunk) {
			gstate.returned.push_back(gcal_map::EventToRow(created, gstate.names, gstate.calendar_id));
		}
	}
	return SinkResultType::NEED_MORE_INPUT;
}

class CalendarInsertSourceState : public GlobalSourceState {
public:
	bool finished = false;
	idx_t offset = 0; // RETURNING: cursor into the collected rows
};

unique_ptr<GlobalSourceState> CalendarInsert::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CalendarInsertSourceState>();
}

SourceResultType CalendarInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CalendarInsertSourceState>();
	auto &sink = sink_state->Cast<CalendarInsertGlobalSinkState>();
	if (return_chunk) {
		gcal_map::EmitReturnedRows(chunk, sink.returned, state.offset);
		return state.offset >= sink.returned.size() ? SourceResultType::FINISHED
		                                             : SourceResultType::HAVE_MORE_OUTPUT;
	}
	if (state.finished) {
		return SourceResultType::FINISHED;
	}
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(sink.insert_count)));
	state.finished = true;
	return SourceResultType::FINISHED;
}

} // namespace duckdb
