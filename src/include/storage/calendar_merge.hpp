#pragma once

#include "duckdb/common/helper.hpp"

namespace duckdb {
class ClientContext;
class PhysicalPlanGenerator;
class LogicalMergeInto;
class BoundMergeIntoAction;
class MergeIntoOperator;
class CalendarTableEntry;

unique_ptr<MergeIntoOperator> PlanCalendarMergeIntoAction(ClientContext &context, LogicalMergeInto &op,
                                                          PhysicalPlanGenerator &planner, CalendarTableEntry &table,
                                                          BoundMergeIntoAction &action);

} // namespace duckdb
