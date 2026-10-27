-- Shared preamble for the fixture-driven tests: the same attachment lib/setup.sql builds, but with
-- an access_token secret instead of the interactive OAuth one, so it runs under the
-- GOOGLE_CALENDAR_TEST_FIXTURE mock seam with no credentials.

.bail on

load google_calendar;

-- json and icu come from lib/helpers.sql, deliberately not from here: see derive_test.sql.
.read examples/lib/helpers.sql

set timezone = 'America/New_York';
.read examples/derivations/personal_to_employer.sql
.read examples/derivations/employer_to_contract.sql
.read examples/derivations/contract_to_employer.sql
.read examples/derivations/employer_to_employer.sql

.once /dev/null
create secret gcal_fixture (type google_calendar, provider access_token, token 'dummy');

set variable pipeline_calendars = (
  select list(distinct calendar_id) from (
    select from_calendar_id as calendar_id from derivations
    union
    select to_calendar_id from derivations
  )
);

attach 'me' as calendar (
  type google_calendar,
  secret gcal_fixture,
  default_window_start '2026-01-01'::date,
  default_window_length interval '14 days',
  default_calendars getvariable('pipeline_calendars')
);

create temporary view base_events as
  from calendar.events
  where calendar_id in (select from_calendar_id from derivations)
;

.read examples/lib/derive.sql
