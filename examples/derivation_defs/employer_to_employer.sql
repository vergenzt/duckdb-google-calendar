insert into derivations values
  ('employer_to_employer', getenv('EMPLOYER_CALENDAR_ID'), getenv('EMPLOYER_CALENDAR_ID'));

create function is_interview(summary, description) as
  summary like 'Interview %' and description like '%greenhouse.io/guides%';

-- Older shape than as_derived_from(), and keyed by which block it is rather than by source
-- calendar, since both blocks come from the same event on the same calendar. Kept as-is so the
-- blocks already on the calendar keep their ids.
create function block_id(block, event_id, cal_id) as
  md5(cal_id) || md5(block || '-for-' || event_id);

create macro derive_employer_to_employer(src) as table
    select
      'employer_to_employer' as derivation,
      getenv('EMPLOYER_CALENDAR_ID') as calendar_id,
      block_id('prep', (src).event_id, (src).calendar_id) as event_id,
      (src).color_id as color_id,
      'Prep: ' || (src).summary as summary,
      (src).description as description,
      (src).start - interval '30 minutes' as start,
      (src).start as "end",
    where
      (src).calendar_id = getenv('EMPLOYER_CALENDAR_ID')
      and is_interview((src).summary, (src).description)

  union all by name

    select
      'employer_to_employer' as derivation,
      getenv('EMPLOYER_CALENDAR_ID') as calendar_id,
      block_id('feedback', (src).event_id, (src).calendar_id) as event_id,
      (src).color_id as color_id,
      'Feedback: ' || (src).summary as summary,
      (src).description as description,
      (src)."end" as start,
      (src)."end" + interval '30 minutes' as "end",
    where
      (src).calendar_id = getenv('EMPLOYER_CALENDAR_ID')
      and is_interview((src).summary, (src).description)
;
