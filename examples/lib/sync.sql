-- callers should declare a temporary view called "src_replicated"

.mode json

merge into calendar.dst
using src_replicated
  on dst.event_id = src_replicated.event_id

  when not matched then insert by name
  when matched then update by name

  when not matched by source
  and dst.event_id.is_replica_from(getvariable('src_cal_id'))
  then delete

  returning merge_action, *
;
