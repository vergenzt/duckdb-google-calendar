-- Write-path test: proves the pipeline actually issues Calendar API writes, rather than binding
-- cleanly and then no-opping.
--
-- The fixture returns the same single event for every events.list call, so each calendar looks
-- like it holds one original event and the pipeline derives onto the others. Every response after
-- the reads is an events.insert reply.
--
--   examples/test/run.sh

.read examples/test/fixture_attach.sql

-- Four events are implied: personal->employer, employer->contract, contract->employer, and the
-- two-step personal->employer->contract. Writing back to a calendar an event came from (the
-- employer->contract result returning to employer, and vice versa) is refused by the
-- `calendars_so_far` check -- so this also confirms the loop protection works on the real catalog
-- path, not just against the stub.
create temporary table expected_inserts as select 4 as n;

.output /dev/null
create temporary table applied as from derived_events select count(*) as n;
.output

select case
  when (from applied select n) = (from expected_inserts select n) then true
  else error(format('expected {} derived events, got {}',
                    (from expected_inserts select n), (from applied select n)))
  end as derived_count_ok;

.mode json
merge into calendar.events as dst
using derived_events as src
  on dst.calendar_id = src.calendar_id
 and dst.event_id = src.event_id
  when not matched then insert by name
  when matched then update by name
  when not matched by source and is_managed(dst.event_id) then delete
  returning merge_action
;

.mode box
select 'Fixture write assertions passed.' as result;
