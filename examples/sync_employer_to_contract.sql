set variable src_cal_id = getenv('EMPLOYER_CALENDAR_ID');
set variable dst_cal_id = getenv('CONTRACT_CALENDAR_ID');
.read examples/lib/setup.sql

create temporary view src_replicated as
  from src_calendar.src
  select
    event_id.as_replica_from(calendar_id) as event_id,
    getenv('REPLICA_COLOR_ID') as color_id,

    case
    when summary.starts_with('[Personal]') then summary
    else format(
      '[{}] {}',
      getenv('EMPLOYER_NAME'),
      summary.regexp_replace('^((?:(?:Prep|Feedback): )?Interview) .*', '\1')
    )
    end as summary,
    '' as description,
    start as start,
    "end" as "end",
  where
    should_replicate(src)
    and not event_id.is_replica_from(getvariable('dst_cal_id'))
;

.read examples/lib/sync.sql
