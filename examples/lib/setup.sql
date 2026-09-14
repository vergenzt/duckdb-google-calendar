.bail on

install json; load json;
install icu; load icu; -- timezone support
load google_calendar;

.once /dev/null
create secret google_oauth (type google_calendar, provider oauth);

set variable default_window_length = interval '14 days';
set variable default_window_start = today();

attach 'me' as calendar (
  type google_calendar,
  secret google_oauth,
  default_window_start getvariable('default_window_start'),
  default_window_length getvariable('default_window_length'),

  -- These scripts only ever touch the two calendars being synced, so scope the catalog to them.
  -- Any query that carries no calendar_id predicate of its own -- notably the merge in lib/sync.sql,
  -- whose `not matched by source` arm blocks predicate pushdown -- then reads just these instead of
  -- every calendar in the account. (Identical src/dst, as in the employer->employer script, is
  -- deduplicated.) A query that *does* name a calendar_id overrides this and can still reach any
  -- other calendar.
  default_calendars [getvariable('src_cal_id'), getvariable('dst_cal_id')]
);

create temporary view src as
  from calendar.events
  where calendar_id = getvariable('src_cal_id')
;

create function self_status(src) as
  (from json_each(src.attendees) select value where (value->'self') = 'true')->>'responseStatus';
;

create function should_replicate(src) as
  src.status is distinct from 'cancelled'
  and self_status(src) is distinct from 'declined'
  and not src.all_day
;

create function as_replica_from(event_id, cal_id) as 'replica' || md5(cal_id) || md5(event_id);

create function is_replica(event_id) as event_id like 'replica%';

create function is_replica_from(event_id, cal_id) as event_id like 'replica' || md5(cal_id) || '%';
