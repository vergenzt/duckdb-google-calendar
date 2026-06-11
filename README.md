# duckdb-google-calendar

A DuckDB extension that exposes Google Calendar as an attachable, read/write catalog
(`ATTACH ... (TYPE google_calendar)`), with one `events` table per calendar.

## Build

    make          # builds the extension + a duckdb shell with it preloaded
    make test     # runs test/sql/*.test (live tests need credentials; see below)

## Authenticate

Create a `google_calendar` secret (one of three providers):

    -- Service account (recommended for automation)
    CREATE SECRET cal (TYPE google_calendar, PROVIDER key_file, filepath '/path/sa.json');

    -- ...or supply the key JSON via an env var (nothing on disk; name only in SQL):
    --   export GCAL_SA_KEY="$(cat sa.json)"   # e.g. via `op run` or a CI secret
    CREATE SECRET cal (TYPE google_calendar, PROVIDER key_file, key_env 'GCAL_SA_KEY');

    -- Pre-obtained OAuth access token
    CREATE SECRET cal (TYPE google_calendar, PROVIDER access_token, token '...');

    -- Interactive OAuth (requires a registered app):
    --   export GOOGLE_CALENDAR_OAUTH_CLIENT_ID=...   # optional: GOOGLE_CALENDAR_OAUTH_REDIRECT_URI
    CREATE SECRET cal (TYPE google_calendar, PROVIDER oauth);

## Attach & query

    ATTACH 'me' AS cal (TYPE google_calendar, SECRET cal);
    SHOW TABLES FROM cal;   -- one table per calendar (the primary calendar is cal.primary)

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
