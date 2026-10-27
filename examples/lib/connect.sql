-- Attaches the calendar catalog and binds `base_events` for lib/derive.sql.
-- Requires lib/helpers.sql and the files in examples/derivations/ to have been read already: the
-- attachment's scope comes from the calendars those derivations name.

load google_calendar;

.once /dev/null
create secret google_oauth (type google_calendar, provider oauth);

set variable default_window_length = interval '14 days';
set variable default_window_start = today();

set variable pipeline_calendars = (
  select list(distinct calendar_id) from (
    -- all *_CALENDAR_ID env vars referenced in derivations
    from read_text('examples/derivations/*.sql')
    select distinct getenv(regexp_extract(content, 'getenv\(''(\w+_CALENDAR_ID)''\)', 1)) as calendar_id
  )
);

attach 'me' as calendar (
  type google_calendar,
  secret google_oauth,
  default_window_start getvariable('default_window_start'),
  default_window_length getvariable('default_window_length'),
  default_calendars getvariable('pipeline_calendars')
);

-- The seam lib/derive.sql reads from. examples/test/derive_test.sql binds this to a stub table
-- instead, which is what makes the pipeline testable without credentials.
--
-- The scan is bounded by `default_calendars` above and by the default window; the filter here only
-- skips calendars that are pure destinations, and is a no-op when every calendar is also a source.
create temporary view base_events as
  from calendar.events
  where calendar_id in (select from_calendar_id from derivations)
;
