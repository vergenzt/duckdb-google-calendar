#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/index_vector.hpp"

namespace duckdb {
class CalendarTableEntry;

class CalendarUpdate : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	CalendarUpdate(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
	               vector<PhysicalIndex> columns, vector<idx_t> value_indices, idx_t estimated_cardinality);

	CalendarTableEntry &table;
	vector<PhysicalIndex> columns;     // SET target schema columns
	vector<idx_t> value_indices;       // parallel: child-chunk index of each SET value

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
