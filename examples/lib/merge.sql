-- Applies `derived_events` (from lib/derive.sql) to the calendars in one statement.
--
-- One MERGE covers every derivation and every destination, because lib/derive.sql has already
-- worked out the complete set the pipeline should have written, chains included. That is what makes
-- the delete arm correct: anything the pipeline owns that this set does not account for is stale, so
-- events left behind by a removed derivation, a changed transform, or a deleted original are all
-- cleaned up here. A per-derivation script could only ever make that judgement about one source
-- calendar at a time.
--
-- Note on cost: `when not matched by source` forces a full scan of the merge target, and DuckDB
-- cannot push `dst.calendar_id in (...)` past it -- so this statement cannot narrow itself the way
-- a plain WHERE would. lib/setup.sql handles that by scoping the attachment with
-- `default_calendars`, which bounds the scan to the calendars the pipeline names. Without that option this
-- would read *every* calendar in the account over the default window (correct either way -- the
-- delete arm is guarded on is_managed -- just far more API calls).

.mode json

merge into calendar.events as dst
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

  when not matched by source and is_managed(dst.event_id) then delete

  returning merge_action, *
;
