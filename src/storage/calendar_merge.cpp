#include "storage/calendar_merge.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_insert.hpp"
#include "storage/calendar_update.hpp"
#include "storage/calendar_delete.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/operator/persistent/physical_merge_into.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

unique_ptr<MergeIntoOperator> PlanCalendarMergeIntoAction(ClientContext &context, LogicalMergeInto &op,
                                                          PhysicalPlanGenerator &planner, CalendarTableEntry &table,
                                                          BoundMergeIntoAction &action) {
	auto result = make_uniq<MergeIntoOperator>();
	result->action_type = action.action_type;
	result->condition = std::move(action.condition);

	auto return_types = op.types;
	if (op.return_chunk) {
		return_types.pop_back();
	}
	auto cardinality = op.EstimateCardinality(context);

	switch (action.action_type) {
	case MergeActionType::MERGE_UPDATE: {
		vector<idx_t> value_indices;
		for (auto &expr : action.expressions) {
			value_indices.push_back(expr->Cast<BoundReferenceExpression>().index);
		}
		result->op = planner.Make<CalendarUpdate>(return_types, table, std::move(action.columns),
		                                          std::move(value_indices), cardinality, op.return_chunk);
		break;
	}
	case MergeActionType::MERGE_DELETE: {
		result->op = planner.Make<CalendarDelete>(return_types, table, op.row_id_start, cardinality, op.return_chunk);
		break;
	}
	case MergeActionType::MERGE_INSERT: {
		result->op = planner.Make<CalendarInsert>(return_types, table, cardinality, op.return_chunk);
		if (!action.column_index_map.empty()) {
			vector<unique_ptr<Expression>> new_expressions;
			for (auto &col : op.table.GetColumns().Physical()) {
				auto storage_idx = col.StorageOid();
				auto mapped_index = action.column_index_map[col.Physical()];
				if (mapped_index == DConstants::INVALID_INDEX) {
					new_expressions.push_back(op.bound_defaults[storage_idx]->Copy());
				} else {
					new_expressions.push_back(std::move(action.expressions[mapped_index]));
				}
			}
			action.expressions = std::move(new_expressions);
		}
		result->expressions = std::move(action.expressions);
		break;
	}
	case MergeActionType::MERGE_ERROR:
		result->expressions = std::move(action.expressions);
		break;
	case MergeActionType::MERGE_DO_NOTHING:
		break;
	default:
		throw InternalException("Unsupported merge action in google_calendar catalog");
	}
	return result;
}

} // namespace duckdb
