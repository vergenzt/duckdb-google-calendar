#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class CalendarTableEntry;

// INSERT INTO calendars -> calendars.insert (POST /calendars). Creating a calendar this way replaces
// the old CREATE TABLE ... AS path, which no longer exists now that the table set is fixed.
class CalendarsInsert : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	CalendarsInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
	                idx_t estimated_cardinality, bool return_chunk);

	CalendarTableEntry &table;
	bool return_chunk;

public:
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
};

// DELETE FROM calendars -> calendars.delete (DELETE /calendars/{id}), guarded so only calendars this
// session created can be removed.
class CalendarsDelete : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	CalendarsDelete(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
	                idx_t row_id_index, idx_t estimated_cardinality, bool return_chunk);

	CalendarTableEntry &table;
	idx_t row_id_index;
	bool return_chunk;

public:
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
};

} // namespace duckdb
