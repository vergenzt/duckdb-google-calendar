-- callers should declare a temporary view called "src_replicated"
-- (it must carry a calendar_id column naming the destination calendar, since `events` spans all of
-- them and `insert by name` takes the destination from the row)
--
-- Note on cost: the `when not matched by source` clause below forces a full scan of the merge
-- target, and DuckDB cannot push the `dst.calendar_id = ...` predicate past it -- so this statement
-- cannot narrow itself the way a plain WHERE would. lib/setup.sql handles that by scoping the
-- attachment with `default_calendars [src, dst]`, which bounds the scan to those two calendars.
-- Without that option this would read *every* calendar in the account over the default window
-- (correct either way -- the delete arm is guarded on calendar_id -- just far more API calls).

.mode json

merge into calendar.events as dst
using src_replicated
  on dst.calendar_id = getvariable('dst_cal_id')
 and dst.event_id = src_replicated.event_id

  when not matched then insert by name
  when matched then update by name

  when not matched by source
  and dst.calendar_id = getvariable('dst_cal_id')
  and dst.event_id.is_replica_from(getvariable('src_cal_id'))
  then delete

  returning merge_action, *
;
