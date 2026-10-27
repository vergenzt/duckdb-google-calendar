-- Credential-free test for lib/derive.sql. Binds `base_events` to a stub instead of
-- calendar.events, so the whole pipeline -- chains of several derivations, the loop protection,
-- every derivation's transform, and the MERGE arms -- is checked with no API calls.
--
--   examples/test/run.sh
--
-- Failures raise, and `.bail on` stops the script on the first one.

.bail on

create macro assert_eq(label, actual, expected) as
  case when actual is not distinct from expected then true
  else error(format('FAIL {}:\n  got:  {}\n  want: {}', label, actual, expected)) end;

create macro assert_true(label, actual) as assert_eq(label, actual, true);

-- No install/load here on purpose: lib/helpers.sql must be self-sufficient, since
-- examples/sync.sql reads it before lib/setup.sql. Loading json or icu in this file would hide a
-- broken read order in the real entry point.
.read examples/lib/helpers.sql

-- Business-hour rules are local-time; pin the zone so the expectations are stable.
set timezone = 'America/New_York';

.read examples/derivations/personal_to_employer.sql
.read examples/derivations/employer_to_contract.sql
.read examples/derivations/contract_to_employer.sql
.read examples/derivations/employer_to_employer.sql

-- 2026-01-05 is a Monday. Stub covers: a medical appointment with a close preceding neighbour, a
-- plain personal event, an interview that the prep/feedback rule fires on, and rows to be skipped.
create table stub_events (
  calendar_id varchar, event_id varchar, summary varchar, description varchar,
  color_id varchar, visibility varchar, transparency varchar,
  start timestamptz, "end" timestamptz, all_day boolean, status varchar, attendees varchar
);

insert into stub_events (calendar_id, event_id, summary, description, start, "end") values
  -- personal
  ('personal@x', 'p_prior',  'Gym',                 null, '2026-01-05 08:30:00-05', '2026-01-05 09:50:00-05'),
  ('personal@x', 'p_dental', 'Dentist appointment', null, '2026-01-05 10:00:00-05', '2026-01-05 11:00:00-05'),
  ('personal@x', 'p_lunch',  'Lunch with Dana',     null, '2026-01-05 12:00:00-05', '2026-01-05 13:00:00-05'),
  -- employer
  ('employer@x', 'e_iv', 'Interview Bob', 'see https://greenhouse.io/guides/123',
                                                          '2026-01-06 14:00:00-05', '2026-01-06 15:00:00-05'),
  ('employer@x', 'e_standup', 'Standup',        null,      '2026-01-06 09:00:00-05', '2026-01-06 09:15:00-05'),
  -- contract
  ('contract@x', 'c_sync', 'Contract sync',     null,      '2026-01-07 11:00:00-05', '2026-01-07 12:00:00-05')
;

-- Rows nothing may be derived from.
insert into stub_events (calendar_id, event_id, summary, start, "end", all_day, status, attendees) values
  ('personal@x', 'p_weekend', 'Saturday brunch', '2026-01-10 11:00:00-05', '2026-01-10 12:00:00-05', false, null, null),
  ('personal@x', 'p_allday',  'Vacation',        '2026-01-05 00:00:00-05', '2026-01-06 00:00:00-05', true,  null, null),
  ('personal@x', 'p_cancel',  'Cancelled thing', '2026-01-05 15:00:00-05', '2026-01-05 16:00:00-05', false, 'cancelled', null),
  ('personal@x', 'p_declined','Declined thing',  '2026-01-05 15:00:00-05', '2026-01-05 16:00:00-05', false, null,
     '[{"self": true, "responseStatus": "declined"}]'),
  ('personal@x', 'p_evening', 'Evening class',   '2026-01-05 19:00:00-05', '2026-01-05 20:00:00-05', false, null, null),
  -- An event the pipeline already wrote, read back from the API: must not be derived from.
  ('employer@x', as_derived_from('p_lunch', 'personal@x'), '[Personal] Lunch with Dana',
     '2026-01-05 12:00:00-05', '2026-01-05 13:00:00-05', false, null, null)
;

create temporary view base_events as from stub_events;

.read examples/lib/derive.sql

-- ---------------------------------------------------------------------------
-- The derived set is complete and correct
-- ---------------------------------------------------------------------------

.mode box
-- Assertions raise on failure, so their `true` results are just noise.
.output /dev/null

-- Whole-set comparison, so any drift in any transform shows up here rather than only in whichever
-- property a targeted assertion happens to look at.
create table expected (calendar_id varchar, summary varchar, start timestamptz, "end" timestamptz);
insert into expected values
  -- personal -> employer. Every weekday business-hours event, retitled; medical ones padded.
  ('employer@x', '[Personal] Gym',             '2026-01-05 08:30:00-05', '2026-01-05 09:50:00-05'),
  ('employer@x', '[Personal] Appointment',     '2026-01-05 09:50:00-05', '2026-01-05 11:30:00-05'),
  ('employer@x', '[Personal] Lunch with Dana', '2026-01-05 12:00:00-05', '2026-01-05 13:00:00-05'),
  -- employer -> employer: the inferred interview blocks.
  ('employer@x', 'Prep: Interview Bob',        '2026-01-06 13:30:00-05', '2026-01-06 14:00:00-05'),
  ('employer@x', 'Feedback: Interview Bob',    '2026-01-06 15:00:00-05', '2026-01-06 15:30:00-05'),
  -- contract -> employer.
  ('employer@x', '[Contract] Contract sync',   '2026-01-07 11:00:00-05', '2026-01-07 12:00:00-05'),
  -- employer -> contract, one step.
  ('contract@x', '[Employer] Standup',         '2026-01-06 09:00:00-05', '2026-01-06 09:15:00-05'),
  ('contract@x', '[Employer] Interview',       '2026-01-06 14:00:00-05', '2026-01-06 15:00:00-05'),
  -- Two derivations_so_far: personal -> employer -> contract, in a single invocation. This is what the
  -- per-derivation scripts could not do without being re-run.
  ('contract@x', '[Personal] Gym',             '2026-01-05 08:30:00-05', '2026-01-05 09:50:00-05'),
  ('contract@x', '[Personal] Appointment',     '2026-01-05 09:50:00-05', '2026-01-05 11:30:00-05'),
  ('contract@x', '[Personal] Lunch with Dana', '2026-01-05 12:00:00-05', '2026-01-05 13:00:00-05'),
  -- Two steps via the rule that writes back to its own calendar: what it infers is itself
  -- carried onward.
  ('contract@x', '[Employer] Prep: Interview',     '2026-01-06 13:30:00-05', '2026-01-06 14:00:00-05'),
  ('contract@x', '[Employer] Feedback: Interview', '2026-01-06 15:00:00-05', '2026-01-06 15:30:00-05')
;

select assert_eq(
  'derived_events matches expected exactly',
  (from (
     select calendar_id, summary, start, "end" from derived_events
     except all
     select calendar_id, summary, start, "end" from expected
   ) select coalesce(list(summary), [])),
  []
);

select assert_eq(
  'expected matches derived_events exactly (nothing missing)',
  (from (
     select calendar_id, summary, start, "end" from expected
     except all
     select calendar_id, summary, start, "end" from derived_events
   ) select coalesce(list(summary), [])),
  []
);

-- ---------------------------------------------------------------------------
-- Loop protection
-- ---------------------------------------------------------------------------

-- The exact-set comparison above already fails if an event is written back to a calendar it came
-- from, since that would add rows. These name the two specific round trips, so a failure says which
-- protection broke rather than just that the output drifted.
--
-- contract_to_employer must not pick up what employer_to_contract just produced: that would put
-- as_derived_from(as_derived_from(e_standup, 'employer@x'), 'contract@x') back onto employer.
select assert_eq(
  'employer -> contract does not come back to employer',
  (from derived_events select count(*)
     where calendar_id = 'employer@x'
       and event_id = as_derived_from(
             as_derived_from('e_standup', 'employer@x'), 'contract@x')),
  0
);

-- ...and the reverse: employer_to_contract must not re-export what contract_to_employer brought in.
select assert_eq(
  'contract -> employer does not go back to contract',
  (from derived_events select count(*)
     where calendar_id = 'contract@x'
       and event_id = as_derived_from(
             as_derived_from('c_sync', 'contract@x'), 'employer@x')),
  0
);

-- The prep/feedback rule writes to its own source calendar, so it is exempt from the check above
-- and instead applies at most once. Twice would give a "Prep: Prep: ..." block.
select assert_eq(
  'the prep/feedback rule does not feed itself',
  (from derived_events select count(*) where summary like 'Prep: Prep:%'
                                          or summary like 'Feedback: Prep:%'),
  0
);

-- ---------------------------------------------------------------------------
-- Ids are byte-identical to the ones the per-derivation scripts wrote
-- ---------------------------------------------------------------------------
-- This is what makes the cutover a no-op instead of a delete-and-recreate of every derived event.

select assert_eq(
  'single-step id is the existing as_derived_from form',
  (from derived_events select event_id where calendar_id = 'employer@x'
                                        and summary = '[Personal] Appointment'),
  'replica' || md5('personal@x') || md5('p_dental')
);

select assert_eq(
  'two-step id applies the same formula twice, so chains agree too',
  (from derived_events select event_id where calendar_id = 'contract@x'
                                        and summary = '[Personal] Appointment'),
  'replica' || md5('employer@x') || md5('replica' || md5('personal@x') || md5('p_dental'))
);

select assert_eq(
  'inferred interview blocks keep their existing unprefixed id',
  (from derived_events select event_id where calendar_id = 'employer@x'
                                        and summary = 'Prep: Interview Bob'),
  md5('employer@x') || md5('prep-for-e_iv')
);

-- ---------------------------------------------------------------------------
-- The MERGE arms
-- ---------------------------------------------------------------------------
-- Same statement as lib/merge.sql, against a stub target: calendar.events is not writable
-- without credentials, but the arms and their guards are.

create table target_events as
  from derived_events select * limit 0;

insert into target_events (calendar_id, event_id, summary, start, "end") values
  -- Drifted: must be updated back to what the pipeline derives, not duplicated.
  ('employer@x', 'replica' || md5('personal@x') || md5('p_dental'), 'stale title',
     '2026-01-05 10:00:00-05', '2026-01-05 11:00:00-05'),
  -- Orphaned (the event it came from is gone): must be deleted.
  ('employer@x', 'replica' || md5('personal@x') || md5('p_deleted'), '[Personal] Ghost',
     '2026-01-05 16:00:00-05', '2026-01-05 16:30:00-05'),
  -- Orphaned interview block in the older unprefixed form: also must be deleted. The
  -- per-derivation script never cleaned these up, because its guard tested is_derived() only.
  ('employer@x', md5('employer@x') || md5('prep-for-e_gone'), 'Prep: Interview Gone',
     '2026-01-06 10:00:00-05', '2026-01-06 10:30:00-05'),
  -- A real, unmanaged event: must be left completely alone.
  ('employer@x', 'e_standup', 'Standup', '2026-01-06 09:00:00-05', '2026-01-06 09:15:00-05')
;

-- DuckDB will not wrap a MERGE in CREATE TABLE AS, so these assert on the resulting state of the
-- target rather than on the RETURNING log. That is the stronger check anyway.
merge into target_events as dst
using derived_events as src
  on dst.calendar_id = src.calendar_id
 and dst.event_id = src.event_id

  when not matched then insert by name

  -- Only issue a write when something actually differs. An UPDATE here is a GET + PUT per event,
  -- so an unguarded `update by name` spends two API calls on every derived event on every run and
  -- reports a row that changed nothing. The comparison is NULL-safe, and folds NULL to the value
  -- Google itself defaults to for the two fields that have one -- a derivation that leaves those
  -- unset would otherwise differ from what the API reports back, on every run, forever.
  --
  -- This list is every non-key column of `derived_events`. Add a column there and it must be added
  -- here too, or changes to it will never be written; examples/test/derive_test.sql guards that.
  when matched and (
    dst.summary, dst.description, dst.color_id,
    coalesce(dst.visibility, 'default'), coalesce(dst.transparency, 'opaque'),
    dst.start, dst."end"
  ) is distinct from (
    src.summary, src.description, src.color_id,
    coalesce(src.visibility, 'default'), coalesce(src.transparency, 'opaque'),
    src.start, src."end"
  ) then update by name

  -- lib/derive.sql produces the *complete* set for every destination, so anything the pipeline
  -- owns that the source does not account for is stale by definition. The per-derivation scripts
  -- could only ever make that judgement about one source calendar at a time.
  when not matched by source and is_managed(dst.event_id) then delete
;

select assert_eq('target now holds exactly the derived events plus the untouched original',
  (from target_events select count(*)), 13 + 1);

select assert_eq('every row the pipeline owns matches what was derived',
  (from (
     select calendar_id, event_id, summary, start, "end" from target_events
       where is_managed(event_id)
     except all
     select calendar_id, event_id, summary, start, "end" from derived_events
   ) select count(*)),
  0);

select assert_eq('the drifted event was updated in place, not duplicated',
  (from target_events select list(summary)
     where event_id = 'replica' || md5('personal@x') || md5('p_dental')),
  ['[Personal] Appointment']);

select assert_eq('the orphaned derived event was deleted',
  (from target_events select count(*)
     where event_id = 'replica' || md5('personal@x') || md5('p_deleted')),
  0);

select assert_eq('the orphaned older-form interview block was deleted too',
  (from target_events select count(*)
     where event_id = md5('employer@x') || md5('prep-for-e_gone')),
  0);

select assert_eq('the unmanaged event was left alone',
  (from target_events select count(*) where event_id = 'e_standup' and summary = 'Standup'), 1);

-- Re-running against unchanged input must not change anything. This is what the repetition inside
-- lib/derive.sql buys, and the reason there is no outer retry loop around the whole script.
create table after_first_merge as from target_events;

merge into target_events as dst
using derived_events as src
  on dst.calendar_id = src.calendar_id
 and dst.event_id = src.event_id
  when not matched then insert by name
  when matched then update by name
  when not matched by source and is_managed(dst.event_id) then delete
;

-- Nothing differs any more, so the guarded arm matches no rows at all -- this is what makes the
-- real pipeline print an empty result when everything is already up to date.
select assert_eq('no row is left differing from what was derived',
  (from target_events as dst join derived_events as src
     on dst.calendar_id = src.calendar_id and dst.event_id = src.event_id
   select count(*)
   where (dst.summary, dst.description, dst.color_id,
          coalesce(dst.visibility, 'default'), coalesce(dst.transparency, 'opaque'),
          dst.start, dst."end")
         is distinct from
         (src.summary, src.description, src.color_id,
          coalesce(src.visibility, 'default'), coalesce(src.transparency, 'opaque'),
          src.start, src."end")),
  0
);

-- Tripwire: the guard above lists derived_events' non-key columns by hand. If a column is added
-- there, this fails and points at the list that needs updating.
select assert_eq('derived_events still has the 9 columns the update guard assumes',
  (select count(*) from (describe derived_events)), 9);

select assert_eq('a second run changes nothing',
  (from (from target_events except all from after_first_merge) select count(*))
    + (from (from after_first_merge except all from target_events) select count(*)),
  0);

.output
select 'All derivation assertions passed.' as result;
