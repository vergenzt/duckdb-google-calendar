# duckdb-google-calendar

A DuckDB extension to read/write Google Calendar events using DuckDB SQL.

## Usage

1. **Authenticate:**

   ```sql
   CREATE SECRET my_gcal_secret (
     	TYPE google_calendar,
     	/* Choose auth method: */
       /* 1. service account creds on disk */
       PROVIDER key_file, PATH '/path/to/sa.json' /* or... */
       /* 2. service account creds JSON in env var (e.g. $MY_SA_JSON_ENV_VAR) */
       PROVIDER key_file, KEY_ENV 'MY_SA_JSON_ENV_VAR' /* or... */
     	/* 3. pre-authenticated access token */
       PROVIDER access_token, TOKEN '...' /* or... */
       /* 4. interactive oauth (requires $GOOGLE_CALENDAR_OAUTH_CLIENT_ID in env) */
       PROVIDER oauth
   );
   ```

2. **Attach:**

   ```sql
   ATTACH 'my_calendar' (TYPE google_calendar, SECRET 'my_gcal_secret');
   ```

3. **Access:**

   ```sql
   SHOW TABLES FROM my_calendar;
   /* ... */
   
   SELECT id, summary, start_time, end_time, 
   ```

   

## Attach & query

    ATTACH 'me' AS cal (TYPE google_calendar, SECRET cal);
       -- one table per calendar (the primary calendar is cal.primary)
    
    -- Reads REQUIRE an explicit time bound on `start`:
    SELECT id, summary, start, "end"
    FROM cal.primary
    WHERE start >= TIMESTAMPTZ '2026-06-01 00:00-04' AND start < TIMESTAMPTZ '2026-07-01 00:00-04';

## Write

    INSERT INTO cal.primary (summary, start, "end")
    VALUES ('Standup', TIMESTAMPTZ '2026-06-10 09:00-04', TIMESTAMPTZ '2026-06-10 09:15-04');
    
    UPDATE cal.primary SET location = 'Room 4' WHERE id = 'abc123';
    DELETE FROM cal.primary WHERE id = 'abc123';
    
    MERGE INTO cal.primary AS t USING staging AS s ON t.id = s.id
      WHEN MATCHED THEN UPDATE SET summary = s.summary
      WHEN NOT MATCHED THEN INSERT (summary, start, "end") VALUES (s.summary, s.start, s."end");

## Schema & limitations

- `events` columns: `id, summary, description, location, status, html_link, created, updated` (VARCHAR);
  `start`, `end` (TIMESTAMP WITH TIME ZONE); `all_day` (BOOLEAN);
  `attendees, recurrence, reminders, conference_data` (VARCHAR raw-JSON passthrough).
- Row identity is the server event `id`; `UPDATE`/`DELETE`/`MERGE` key on it.
- A time bound on `start` is required for reads (unbounded scans error).
- `UPDATE` is read-modify-write (GET + PUT); `id/html_link/created/updated` are read-only.
- `RETURNING` is not supported; writes are serialized; transient 429/5xx are retried with backoff.
- Credential-free mock-backed sqllogictests are deferred to v2; `test/sql/live.test` exercises the
  live API when `GOOGLE_CALENDAR_ACCESS_TOKEN` is set.

## Contributing

1. `git clone --recursive vergenzt/duckdb-google-calendar && cd duckdb-google-calendar`
2. Compile the project with `make release` (or just `make`; it's the default).
3. Run tests with `make test`.
