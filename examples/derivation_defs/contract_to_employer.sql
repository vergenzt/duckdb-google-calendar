insert into derivations values
  ('contract_to_employer', getenv('CONTRACT_CALENDAR_ID'), getenv('EMPLOYER_CALENDAR_ID'));

create macro derive_contract_to_employer(src) as table
  select
    'contract_to_employer' as derivation,
    getenv('EMPLOYER_CALENDAR_ID') as calendar_id,
    (src).event_id.as_derived_from((src).calendar_id) as event_id,
    getenv('REPLICA_COLOR_ID') as color_id,
    (src).visibility as visibility,
    (src).transparency as transparency,
    format('[{}] {}', getenv('CONTRACT_NAME'), (src).summary) as summary,
    (src).description as description,
    (src).start as start,
    (src)."end" as "end",
  where (src).calendar_id = getenv('CONTRACT_CALENDAR_ID')
;
