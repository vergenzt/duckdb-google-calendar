#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class CalendarTableEntry;

class CalendarInsert : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	CalendarInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
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

} // namespace duckdb
