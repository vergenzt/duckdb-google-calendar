.bail on

install json; load json;
install icu; load icu; -- timezone support

create function as_derived_from(event_id, cal_id, as := '') as (
  'replica' || "as" || md5(cal_id) || md5(event_id)
);

create function is_derived(event_id) as (
  event_id like 'replica%'
);

-- Every id shape the pipeline can produce, and therefore everything the MERGE's
-- `when not matched by source` arm is allowed to delete. The bare-hex branch covers the interview
-- blocks' older unprefixed form, whose stale events the previous per-derivation script could not
-- clean up (its guard tested is_derived only).
--
-- That branch is the loose one: a hand-created event whose id happens to be exactly 64 lowercase
-- hex characters would be treated as ours, and deleted. Google-assigned ids are base32hex and far
-- shorter, so this is only a hazard for ids you mint yourself -- but it is the reason a new
-- derivation should build ids with as_derived_from() rather than adding another bare-hex shape.
create function is_managed(event_id) as
  is_derived(event_id) or event_id similar to '[0-9a-f]{64}'
;

-- ---------------------------------------------------------------------------
-- Shared eligibility
-- ---------------------------------------------------------------------------
-- Applied once to the original events in lib/derive.sql, not per derivation.

create function self_status(attendees) as
  (from json_each(attendees) select value where (value->'self') = 'true')->>'responseStatus';
;

create function should_derive_from(status, attendees, all_day) as
  status is distinct from 'cancelled'
  and self_status(attendees) is distinct from 'declined'
  and not coalesce(all_day, false)
;

-- ---------------------------------------------------------------------------
-- The shape of a derived event
-- ---------------------------------------------------------------------------
-- Zero rows, canonical columns. lib/derive.sql unions this ahead of the derivation macros (by
-- name), so every column exists even when no derivation sets it -- which is what lets a derivation
-- file select only the fields it cares about and leave the rest NULL.
create macro derived_row_template() as table
  select
    null::varchar     as derivation,
    null::varchar     as calendar_id,
    null::varchar     as event_id,
    null::varchar     as summary,
    null::varchar     as description,
    null::varchar     as color_id,
    null::varchar     as visibility,
    null::varchar     as transparency,
    null::timestamptz as start,
    null::timestamptz as "end",
  where false
;
