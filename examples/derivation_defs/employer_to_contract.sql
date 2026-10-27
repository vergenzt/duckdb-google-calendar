-- employer -> contract: show the contract calendar that the time is taken, with interview titles
-- stripped down to the bare stage so candidate names do not leak across clients.

insert into derivations values
  ('employer_to_contract', getenv('EMPLOYER_CALENDAR_ID'), getenv('CONTRACT_CALENDAR_ID'));

create macro derive_employer_to_contract(src) as table
  select
    'employer_to_contract' as derivation,
    getenv('CONTRACT_CALENDAR_ID') as calendar_id,
    as_derived_from((src).event_id, (src).calendar_id) as event_id,
    getenv('REPLICA_COLOR_ID') as color_id,
    (src).visibility as visibility,
    (src).transparency as transparency,

    case
    when starts_with((src).summary, '[Personal]') then (src).summary
    else format(
      '[{}] {}',
      getenv('EMPLOYER_NAME'),
      regexp_replace((src).summary, '^((?:(?:Prep|Feedback): )?Interview) .*', '\1')
    )
    end as summary,

    (src).start as start,
    (src)."end" as "end",

  where (src).calendar_id = getenv('EMPLOYER_CALENDAR_ID')
;
