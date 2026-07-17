set variable src_cal_id = getenv('EMPLOYER_CALENDAR_ID');
set variable dst_cal_id = getenv('EMPLOYER_CALENDAR_ID');
.read examples/lib/setup.sql

create temporary view src_replicated as

  with interviews as (
    from calendar.src
    where
      should_replicate(src)
      and not event_id.is_replica_from(getvariable('dst_cal_id'))
      and ( /* interview-specific logic */
        summary like 'Interview %'
        and description like '%greenhouse.io/guides%'
      )
  )

  from (
    from interviews
    select
      md5(calendar_id) || md5('prep-for-' || event_id) as event_id,
      color_id as color_id,
      'Prep: ' || summary as summary,
      start - interval '30 minutes' as start,
      start as "end",
      description as description

    union all

    from interviews
    select
      md5(calendar_id) || md5('feedback-for-' || event_id) as event_id,
      color_id as color_id,
      'Feedback: ' || summary as summary,
      "end" as start,
      "end" + interval '30 minutes' as "end",
      description as description
  )

;

.read examples/lib/sync.sql
