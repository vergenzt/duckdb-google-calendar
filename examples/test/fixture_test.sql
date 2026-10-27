-- Binding test for the real catalog path. examples/test/derive_test.sql covers the transforms
-- against a stub; this covers what a stub cannot -- that calendar.events works as the relation
-- lib/derive.sql reads *and* as the MERGE target in the same statement.
--
-- Driven by the empty-account fixture, so nothing is derived and no writes are attempted.
-- That makes it independent of the order the executor runs the two scans in.
--
--   examples/test/run.sh

.read examples/test/fixture_attach.sql

select case when (from derived_events select count(*)) = 0 then true
       else error('expected nothing to be derived from an empty account') end as base_scan_ok;

-- The MERGE binds with the recursively-built view as its source and calendar.events as its
-- target. `not matched by source` makes this scan the target too, which is the combination that
-- cannot be exercised against a stub.
.read examples/lib/merge.sql

select 'Fixture binding assertions passed.' as result;
