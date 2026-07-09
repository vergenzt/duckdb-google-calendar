-- set variable src_cal_id = getenv('PERSONAL_CALENDAR_ID');
-- set variable dst_cal_id = getenv('EMPLOYER_CALENDAR_ID');
-- .read examples/lib/setup.sql

-- create or replace function filter_src(src) as
-- ;

-- create temporary view src as
--   from src_calendar.src

--   where should_replicate(src)
--   and not src.all_day
--   and ( /* business hours */
--     src.start::timetz between timetz '8:00' and timetz '17:00' or
--     src."end"::timetz between timetz '8:00' and timetz '17:00'
--   )
--   and extract('dayofweek' from src.start) between 1 /* monday */ and 5 /* friday */

--   select
--     event_id.as_replica_from(calendar_id) as event_id,
--     'OOO' as summary,
--     '' as description,
--     start - interval '15 minutes' as start,
--     "end" + interval '15 minutes' as "end",
-- ;

-- merge into dst_calendar.dst
-- using (
--   with src as (
--     from src_calendar.src
--     where filter_src(src)
--     and should_replicate(src)
--     and not event_id.is_replica_from(getvariable('dst_cal_id'))
--   )
--   select * from map_src(src)
-- ) as src
--   on dst.event_id = md5(src.calendar_id) || md5(src.event_id)

--   when not matched then insert (event_id, start, "end", summary)
--     values (
--       md5(getvariable('src_cal_id')) || md5(src.event_id),
--       src.*
--     )

--   when matched then update set
--     summary = src.summary,
--     description = src.description,
--     start = src.start,
--     "end" = src."end",

--   when not matched by source
--   and dst.event_id.is_replica_from(getvariable('src_cal_id'))
--   then delete

--   returning merge_action, *
-- ;
