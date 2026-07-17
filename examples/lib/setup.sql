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
  calendar_aliases map {
    'personal': getenv('PERSONAL_CALENDAR_ID'),
    'employer': getenv('EMPLOYER_CALENDAR_ID'),
    'contract': getenv('CONTRACT_CALENDAR_ID'),
    'src': getvariable('src_cal_id'),
    'dst': getvariable('dst_cal_id'),
  }
);

create function self_status(src) as
  (from json_each(src.attendees) select value where (value->'self') = 'true')->>'responseStatus';
;

create function should_replicate(src) as
  src.status is distinct from 'cancelled'
  and self_status(src) is distinct from 'declined'
;

create function as_replica_from(event_id, cal_id) as md5(cal_id) || md5(event_id);

create function is_replica_from(event_id, cal_id) as event_id like md5(cal_id) || '%';
