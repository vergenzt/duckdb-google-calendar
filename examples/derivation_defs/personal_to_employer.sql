insert into derivations values
  ('personal_to_employer', getenv('PERSONAL_CALENDAR_ID'), getenv('EMPLOYER_CALENDAR_ID'));

create function is_medical(summary) as
  lower(summary) similar to '.*\b(therap(y|ist)|counseling|appointment|consult)\b.*';

-- Business hours, evaluated in the host's local time zone.
create function workday_start(ts) as date_trunc('day', ts) + interval '08 hours';
create function workday_end(ts) as date_trunc('day', ts) + interval '17 hours';

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

-- Move `val` by `extend_by`, but not past `bound_val` if that bound lies in the way.
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

create macro derive_personal_to_employer(src) as table
  select
    'personal_to_employer' as derivation,
    getenv('EMPLOYER_CALENDAR_ID') as calendar_id,
    (src).event_id.as_derived_from((src).calendar_id) as event_id,
    getenv('REPLICA_COLOR_ID') as color_id,

    'private' as visibility,
    (src).transparency as transparency,

    '[Personal] ' || coalesce(
      case when is_medical((src).summary) then 'Appointment' end,
      (src).summary
    ) as summary,

    case when is_medical((src).summary)
    then extend_bounded(
      (src).start,
      /* extend start time of appointment back by 15 minutes */
      interval '-15 minutes',
      /* ...but no earlier than start of workday (or end of previous appointment) */
      greatest(
        workday_start((src).start),
        (src).prev_end
      )
    )
    else (src).start
    end as start,

    case when is_medical((src).summary)
    then extend_bounded(
      (src)."end",
      /* extend end time of appointment out by 30 minutes */
      interval '30 minutes',
      /* ...but no later than end of workday (or start of next appointment) */
      least(
        workday_end((src).start),
        (src).next_start
      )
    )
    else (src)."end"
    end as "end",

  where
    (src).calendar_id = getenv('PERSONAL_CALENDAR_ID')
    and (src).start.isodow() <= 5
    and list_bool_or(apply(
      [(src).start, (src)."end"],
      lambda ts: ts >= workday_start((src).start) and ts < workday_end((src).start)
    ))
;
