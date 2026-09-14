#include "storage/calendars_dml.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_transaction.hpp"
#include "storage/calendar_catalog.hpp"
#include "storage/calendar_mapping.hpp"
#include "storage/event_mapping.hpp"

#include "calendar/client.hpp"

namespace duckdb {

// ---------- INSERT ----------

CalendarsInsert::CalendarsInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
                                 idx_t estimated_cardinality, bool return_chunk)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table), return_chunk(return_chunk) {
}

class CalendarsInsertGlobalSinkState : public GlobalSinkState {
public:
	gcal::GoogleCalendarClient *client = nullptr;
	CalendarCatalog *catalog = nullptr;
	vector<string> names; // chunk column index -> schema column name
	idx_t insert_count = 0;
	vector<vector<Value>> returned; // RETURNING: one full-schema row per created calendar
};

unique_ptr<GlobalSinkState> CalendarsInsert::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<CalendarsInsertGlobalSinkState>();
	auto &transaction = CalendarTransaction::Get(context, table.ParentCatalog());
	state->client = &transaction.GetClient(context);
	state->catalog = &table.ParentCatalog().Cast<CalendarCatalog>();
	for (auto &col : table.GetColumns().Logical()) {
		state->names.push_back(col.Name());
	}
	return std::move(state);
}

SinkResultType CalendarsInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CalendarsInsertGlobalSinkState>();
	chunk.Flatten();
	for (idx_t row = 0; row < chunk.size(); row++) {
		auto body = gcal_map::RowToCalendar(chunk, row, gstate.names);
		gcal::CalendarListEntry created;
		try {
			created = gstate.client->Calendars().Insert(body);
		} catch (const std::exception &e) {
			throw IOException("google_calendar: calendars.insert failed: %s\nrequest body: %s", e.what(), body.dump());
		}
		// Only calendars created here may later be deleted, so record it before anything else can fail.
		gstate.catalog->MarkCreatedThisSession(created.id);
		gstate.insert_count++;
		if (return_chunk) {
			gstate.returned.push_back(gcal_map::CalendarToRow(created, gstate.names));
		}
	}
	return SinkResultType::NEED_MORE_INPUT;
}

class CalendarsInsertSourceState : public GlobalSourceState {
public:
	bool finished = false;
	idx_t offset = 0; // RETURNING: cursor into the collected rows
};

unique_ptr<GlobalSourceState> CalendarsInsert::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CalendarsInsertSourceState>();
}

SourceResultType CalendarsInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                  OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CalendarsInsertSourceState>();
	auto &sink = sink_state->Cast<CalendarsInsertGlobalSinkState>();
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

// ---------- DELETE ----------

CalendarsDelete::CalendarsDelete(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
                                 idx_t row_id_index, idx_t estimated_cardinality, bool return_chunk)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table), row_id_index(row_id_index), return_chunk(return_chunk) {
}

class CalendarsDeleteGlobalSinkState : public GlobalSinkState {
public:
	gcal::GoogleCalendarClient *client = nullptr;
	CalendarCatalog *catalog = nullptr;
	vector<string> names; // only needed for RETURNING
	idx_t delete_count = 0;
	vector<vector<Value>> returned;
};

unique_ptr<GlobalSinkState> CalendarsDelete::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<CalendarsDeleteGlobalSinkState>();
	auto &transaction = CalendarTransaction::Get(context, table.ParentCatalog());
	state->client = &transaction.GetClient(context);
	state->catalog = &table.ParentCatalog().Cast<CalendarCatalog>();
	if (return_chunk) {
		for (auto &col : table.GetColumns().Logical()) {
			state->names.push_back(col.Name());
		}
	}
	return std::move(state);
}

SinkResultType CalendarsDelete::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CalendarsDeleteGlobalSinkState>();
	chunk.Flatten();
	for (idx_t row = 0; row < chunk.size(); row++) {
		auto id_val = chunk.GetValue(row_id_index, row);
		if (id_val.IsNull()) {
			continue;
		}
		// The calendars rowid is the calendar id itself.
		string calendar_id = id_val.ToString();

		// Guard: only calendars this session created (via INSERT INTO calendars) may be deleted, so a
		// script can clean up after itself but a stray DELETE can never destroy a real user calendar.
		if (!gstate.catalog->WasCreatedThisSession(calendar_id)) {
			throw InvalidInputException(
			    "google_calendar: refusing to delete calendar \"%s\": only calendars created in this session "
			    "with INSERT INTO calendars can be deleted (this protects pre-existing calendars)",
			    calendar_id);
		}

		// RETURNING must report the deleted rows; the child only carries the row id, so fetch the entry
		// before removing it.
		if (return_chunk) {
			auto entry = gstate.client->CalendarList().Get(calendar_id);
			gstate.returned.push_back(gcal_map::CalendarToRow(entry, gstate.names));
		}
		gstate.client->Calendars().Delete(calendar_id);
		gstate.catalog->ForgetCreatedThisSession(calendar_id);
		gstate.delete_count++;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

class CalendarsDeleteSourceState : public GlobalSourceState {
public:
	bool finished = false;
	idx_t offset = 0;
};

unique_ptr<GlobalSourceState> CalendarsDelete::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CalendarsDeleteSourceState>();
}

SourceResultType CalendarsDelete::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                  OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CalendarsDeleteSourceState>();
	auto &sink = sink_state->Cast<CalendarsDeleteGlobalSinkState>();
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
