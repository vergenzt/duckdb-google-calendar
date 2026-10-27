-- Builds `derived_events`: every event the pipeline implies, including the ones that only appear
-- after a chain of derivations, repeated until nothing further comes out.
--
-- Requires `base_events` (a view over the events on the calendars the pipeline reads -- lib/setup.sql
-- binds it to calendar.events, examples/test/derive_test.sql to a stub) and the `derive_*(src)`
-- macros from examples/derivations/.
--
-- Everything here is generic: no derivation is named except in the dispatch list below, and the
-- loop protection belongs to the pipeline rather than to any derivation, so a derivation file never
-- has to reason about the others.
--
-- Note what is repeated and what is not. The repetition is over *what should exist*, not over
-- writes: a recursive CTE cannot contain DML, and does not need to, since derived events are a pure
-- function of the original ones. So this works out the full multi-step result up front and a single
-- MERGE applies it. Nothing is read back after being written, and the result is settled by
-- construction rather than by running the pipeline again and seeing that nothing changed.

-- Only original events start things off: a derived event read back from the API would otherwise be
-- derived from a second time, producing a duplicate under a fresh id alongside the copy already
-- built from its original.
--
-- `prev_end` / `next_start` are the neighbouring events on the same calendar, for derivations that
-- pad an event and must not create a collision. They are worked out here rather than inside the
-- recursive term because DuckDB, which does permit a window function there, would scope it to the
-- rows produced by the previous step -- silently making them "neighbours among rows added at this
-- step" instead of neighbours on the calendar.
create temporary view source_events as
  from (
    from base_events
    select
      calendar_id, event_id, summary, description, color_id, visibility, transparency,
      start, "end", all_day, status, attendees,
      lag("end") over (partition by calendar_id order by start, event_id) as prev_end,
      lead(start) over (partition by calendar_id order by start, event_id) as next_start,
  )
  select
    calendar_id, event_id, summary, description, color_id, visibility, transparency,
    start, "end", prev_end, next_start,
  where
    not is_managed(event_id)
    and should_derive_from(status, attendees, all_day)
;
--
-- `seen` is bookkeeping for the repetition and is dropped on the way out -- `insert by name` and
-- `update by name` reject a source carrying columns the events table does not have.
create temporary view derived_events as
  with recursive derived_events (
    calendar_id, event_id, summary, description, color_id, visibility, transparency,
    start, "end", prev_end, next_start, seen
  )
  using key (calendar_id, event_id)
  as (
      from source_events
      select
        calendar_id, event_id, summary, description, color_id, visibility, transparency,
        start, "end", prev_end, next_start,
        -- Seeded with the origin calendar, so an event can never be written back to it.
        [calendar_id] as seen,

    union all

      -- a recursive CTE may refer to itself only once, so derivations are dispatched through a single lateral
      from derived_events as curr, lateral (
        from derived_row_template()
        union all by name from derive_personal_to_employer(curr)
        union all by name from derive_employer_to_contract(curr)
        union all by name from derive_contract_to_employer(curr)
        union all by name from derive_employer_to_employer(curr)
      ) as next

      select
        next.calendar_id, next.event_id, next.summary, next.description, next.color_id, next.visibility,
        next.transparency, next.start, next."end",

        -- Neighbour bounds are only meaningful for original events. A derivation that consults
        -- them should read from a calendar nothing is written to, so it only ever sees step-0 rows.
        null as prev_end,
        null as next_start,

        -- What this step must not repeat: the destination calendar, or -- when a derivation
        -- writes back to its own source calendar, where the destination is on the list by
        -- definition -- the derivation's own name, so it applies at most once.
        -- Aliased `seen_next`, not `seen`: the CTE column list renames it positionally, and an
        -- unqualified `seen` in the WHERE below would silently resolve to curr's *input* list
        -- instead of this one -- which reads as distinct by induction, so the check would pass for
        -- every row and drop the loop protection entirely.
        list_append(
          curr.seen,
          if(next.calendar_id = curr.calendar_id, next.derivation, next.calendar_id)
        ) as seen_next,

      where
        -- One list, one rule: the step must add something not already there. Only the newly
        -- appended element can be a duplicate, so this is exactly "mark not already present".
        len(list_distinct(seen_next)) = len(seen_next)
  )
  from derived_events
  select
    calendar_id,
    event_id,
    summary,
    description,
    color_id,
    visibility,
    transparency,
    start,
    "end",
  -- Originals hold just their origin calendar; anything longer was derived.
  where len(seen) > 1
;
