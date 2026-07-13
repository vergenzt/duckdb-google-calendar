# duckdb-google-calendar

A DuckDB extension that exposes Google Calendar as an attachable, read/write catalog
(`ATTACH ... (TYPE google_calendar)`), with one `events` table per calendar.

## Build

    make          # builds the extension + a duckdb shell with it preloaded
    make test     # runs test/sql/*.test (live tests need credentials; see below)

## Authenticate

This extension talks to the Google Calendar API under the scope
`https://www.googleapis.com/auth/calendar`. If you have never used Google Cloud
before, do the one-time project setup below, then pick **one** of the three
credential providers.

### One-time Google Cloud setup

1. **Create a project.** Go to <https://console.cloud.google.com/projectcreate>,
   name it (e.g. `duckdb-calendar`), and click *Create*. Select it as the active
   project in the top bar.
2. **Enable the Calendar API.** Visit
   <https://console.cloud.google.com/apis/library/calendar-json.googleapis.com>,
   make sure your new project is selected, and click *Enable*.

That is all that is shared across providers. Each provider below adds its own
credential on top of these two steps.

### Provider 1 — Service account (recommended for automation)

A service account is a robot identity with its own email address. Nothing
interactive happens at query time, so this is the right choice for cron jobs,
CI, and servers.

1. Go to <https://console.cloud.google.com/iam-admin/serviceaccounts>, click
   *Create service account*, give it a name, and click *Done*.
2. Open the account → *Keys* tab → *Add key* → *Create new key* → **JSON**. A
   `.json` key file downloads; keep it secret.
3. **Grant it access to a calendar.** A service account only sees calendars
   shared with it. In Google Calendar (web) open *Settings → Settings for my
   calendars → <your calendar> → Share with specific people*, add the service
   account's email address, and give it *Make changes to events* (needed for
   writes). For domain-wide access instead, an admin can grant domain-wide
   delegation in the Admin console.

Then create the secret from the key file:

    CREATE SECRET cal (TYPE google_calendar, PROVIDER key_file, filepath '/path/sa.json');

Or keep the key out of SQL and off disk by passing its JSON via an env var
(only the variable *name* appears in SQL):

    export GCAL_SA_KEY="$(cat sa.json)"   # e.g. via `op run` or a CI secret
    CREATE SECRET cal (TYPE google_calendar, PROVIDER key_file, key_env 'GCAL_SA_KEY');

### Provider 2 — Interactive OAuth (best for your own calendar)

This logs in as **you** through a browser, so no calendar sharing is needed. It
uses a loopback auth-code flow with PKCE and requires a "Desktop app" OAuth
client.

1. **Configure the consent screen** at
   <https://console.cloud.google.com/apis/credentials/consent>: choose
   *External* (or *Internal* for a Workspace org), fill in the required app
   name/email, and add your own account under *Test users* while the app is in
   testing.
2. **Create the client** at
   <https://console.cloud.google.com/apis/credentials> → *Create credentials* →
   *OAuth client ID* → application type **Desktop app**. Note the client ID and
   client secret.
3. Export them and create the secret. The extension opens a browser, captures
   the redirect on a local `127.0.0.1` listener, and exchanges the code for a
   token:

        export GOOGLE_CALENDAR_OAUTH_CLIENT_ID=...
        export GOOGLE_CALENDAR_OAUTH_CLIENT_SECRET=...      # Desktop-app clients
        # optional: pin a fixed loopback port (default is an ephemeral port);
        # add this exact URI to the client's Authorized redirect URIs if you set it
        export GOOGLE_CALENDAR_OAUTH_REDIRECT_URI=http://127.0.0.1:8085
        CREATE SECRET cal (TYPE google_calendar, PROVIDER oauth);

### Provider 3 — Pre-obtained access token

If you already have a valid OAuth access token (e.g. from `gcloud auth
print-access-token` on an account with the Calendar scope, or another tool),
pass it directly. Tokens are short-lived and are not refreshed for you, so this
is mainly for quick tests:

    CREATE SECRET cal (TYPE google_calendar, PROVIDER access_token, token '...');

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
