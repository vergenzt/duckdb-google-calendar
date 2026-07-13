-- callers should declare a temporary view called "src_replicated"

merge into dst_calendar.dst
using src_replicated
  on dst.event_id = src_replicated.event_id

  when not matched then insert (event_id, summary, description, start, "end")
    values (
      src_replicated.event_id,
      src_replicated.summary,
      src_replicated.description,
      src_replicated.start,
      src_replicated."end",
    )

  when matched and list_bool_or([
    dst.summary is distinct from src_replicated.summary,
    dst.description is distinct from src_replicated.description,
    dst.start is distinct from src_replicated.start,
    dst."end" is distinct from src_replicated."end",
  ])
  then update set
    summary = src_replicated.summary,
    description = src_replicated.description,
    start = src_replicated.start,
    "end" = src_replicated."end",

  when not matched by source
  and dst.event_id.is_replica_from(getvariable('src_cal_id'))
  then delete

  returning merge_action, *
;
