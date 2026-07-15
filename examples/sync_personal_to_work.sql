
set variable src_cal_id = getenv('PERSONAL_CALENDAR_ID');
set variable dst_cal_id = getenv('EMPLOYER_CALENDAR_ID');
.read examples/lib/setup.sql

create function signed_extremum(sign, a, b) as
  case sign
  when +1 then greatest(a, b)
  when -1 then least(a, b)
  else error('sign must be positive or negative')
  end
;

create function value_or_signed_timestamp_infinity(timestamp_val, sign) as
  coalesce(
    timestamp_val,
    case sign
    when -1 then '-infinity'::timestamptz
    when  1 then ' infinity'::timestamptz
    else error('Invalid sign for infinity: ' || sign)
    end
  )
;

create function extend_bounded(val, extend_by, bound_val) as
  signed_extremum(
    /* sign := */ -1 * sign(epoch(extend_by)) /* *least* in the direction of extend_by */,
    /* a := */ val + extend_by /* compare: adding extend_by directly */,
    /* b := */ ( /* compare: the bound_val, but only if it's on the `val + extend_by` side of `val` */
      signed_extremum(
        /* sign := */ sign(epoch(extend_by)) /* *most* in the direction of extend_by */,
        /* a := */ value_or_signed_timestamp_infinity(bound_val, sign(epoch(extend_by))),
        /* b := */ val
      )
    )
  )
;

-- business hours are evaluated in host's local time zone
create temporary view src_replicated as
  from src_calendar.src

  select
    event_id.as_replica_from(calendar_id) as event_id,
    getenv('REPLICA_COLOR_ID') as color_id,
    '' as description,

    lower(summary) similar to '.*\b(therapy|counseling|appointment)\b.*' as is_medical,

    coalesce(case when is_medical then 'OOO for Appointment' end, summary) as summary,

    case when is_medical
    then
      greatest(
        /* extend start time of appointment back by 15 minutes */
        extend_bounded(start, interval '-15 minutes', lag("end") over (order by start)),
        /* ...but no earlier than start of workday */
        date_trunc('day', start) + interval '08 hours'
      )
    else
      start
    end
    as start,

    case when is_medical
    then
      least(
        /* extend end time of appointment out by 30 minutes */
        extend_bounded("end", interval '30 minutes', lead(start) over (order by "end")),
        /* ...but no later than end of workday */
        date_trunc('day', start) + interval '17 hours'
      )
    else
      "end"
    end as "end",

    date_trunc('day', start) + interval '08 hours' as workday_start,
    date_trunc('day', start) + interval '17 hours' - interval '1 second' as workday_end

  where
    should_replicate(src)
    and not event_id.is_replica_from(getvariable('dst_cal_id'))
    and not all_day
    and isodow(start) <= 5
    and list_bool_or(apply([start, "end"], lambda ts: ts between workday_start and workday_end))
;

-- .read examples/lib/sync.sql
