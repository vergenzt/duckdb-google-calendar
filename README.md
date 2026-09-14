# duckdb-google-calendar

A DuckDB extension that exposes Google Calendar as an attachable, read/write catalog
(`ATTACH ... (TYPE google_calendar)`). Every attachment exposes exactly two tables:
`calendars` (the account's calendar list) and `events` (every event on every one
of those calendars, keyed by `calendar_id`).

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
    SHOW TABLES FROM cal;   -- always exactly two: calendars, events

    -- Which calendars can this credential see?
    SELECT calendar_id, summary, access_role, is_primary FROM cal.calendars;

    -- Reads REQUIRE an explicit time bound on `start`:
    SELECT calendar_id, event_id, summary, start, "end"
    FROM cal.events
    WHERE start >= TIMESTAMPTZ '2026-06-01 00:00:00-04'
      AND start < TIMESTAMPTZ '2026-07-01 00:00:00-04';

`events` spans every calendar, so the query above issues one `events.list` per
calendar. Add a `calendar_id` predicate to narrow it — `calendar_id = '...'` and
`calendar_id IN (...)` are pushed down, and the scan then hits only those
calendars (skipping the calendar-list lookup entirely, so an explicitly named
calendar works even if the credential is not subscribed to it):

    SELECT event_id, summary, start FROM cal.events
    WHERE calendar_id = 'you@example.com'
      AND start >= TIMESTAMPTZ '2026-06-01 00:00:00-04'
      AND start < TIMESTAMPTZ '2026-07-01 00:00:00-04';

### Scoping an attachment to specific calendars

If an attachment only ever concerns a few calendars, name them at ATTACH time with
`default_calendars`. The `events` fan-out is then limited to those, and the
calendar-list lookup is skipped entirely:

    ATTACH 'me' AS cal (TYPE google_calendar, SECRET cal,
      default_calendars ['you@example.com', getenv('TEAM_CALENDAR_ID')]);

It takes a single id or a list, and duplicates are collapsed. Like the default
window, it only applies when the query says nothing more specific: a
`calendar_id` predicate **overrides** it rather than intersecting with it, so a
calendar outside the scope is still reachable when named explicitly.

This is the only way to narrow a statement whose predicate DuckDB cannot push
down — most importantly a `MERGE` with a `WHEN NOT MATCHED BY SOURCE` arm, which
by definition must visit every target row and so cannot use the `ON` clause to
restrict the scan.

It does **not** affect the `calendars` table, which always reports the account's
full calendar list — that is how you discover ids, and it is a single cheap call.
(One consequence: if the scope names a calendar you are not subscribed to, its
events appear in `events` without a matching `calendars` row.)

Because the two tables share the `calendar_id` key, joining them works as you'd expect:

    SELECT c.summary AS calendar, count(*) AS events
    FROM cal.events e JOIN cal.calendars c USING (calendar_id)
    WHERE e.start >= TIMESTAMPTZ '2026-06-01 00:00:00-04'
      AND e.start < TIMESTAMPTZ '2026-07-01 00:00:00-04'
    GROUP BY 1 ORDER BY 2 DESC;

> **Note:** a `TIMESTAMPTZ` literal that carries an explicit UTC offset but omits
> seconds (`'2026-06-01 00:00-04'`) is not recognised as a time bound, and the
> scan will reject the query as unbounded. Write the seconds
> (`'2026-06-01 00:00:00-04'`) or drop the offset. This is a long-standing quirk,
> not new to the two-table layout.

## Write

`calendar_id` selects the destination calendar, so it is required on INSERT:

    INSERT INTO cal.events (calendar_id, summary, start, "end")
    VALUES ('you@example.com', 'Standup',
            TIMESTAMPTZ '2026-06-10 09:00:00-04', TIMESTAMPTZ '2026-06-10 09:15:00-04');

`UPDATE`/`DELETE`/`MERGE` find the owning calendar on their own — row identity
carries it — so they need no `calendar_id` predicate, though adding one narrows
the scan they run first:

    UPDATE cal.events SET location = 'Room 4' WHERE event_id = 'abc123';
    DELETE FROM cal.events WHERE event_id = 'abc123';

    MERGE INTO cal.events AS t USING staging AS s
      ON t.calendar_id = s.calendar_id AND t.event_id = s.event_id
      WHEN MATCHED THEN UPDATE SET summary = s.summary
      WHEN NOT MATCHED THEN INSERT (calendar_id, summary, start, "end")
           VALUES (s.calendar_id, s.summary, s.start, s."end");

Creating and removing calendars is `INSERT`/`DELETE` on `calendars`:

    INSERT INTO cal.calendars (summary, time_zone) VALUES ('Scratch', 'UTC')
      RETURNING calendar_id;

    DELETE FROM cal.calendars WHERE calendar_id = '...';

As a safety measure, `DELETE FROM calendars` only accepts calendars **created in
the same session** by `INSERT INTO calendars`; anything pre-existing is refused,
so a stray `DELETE` cannot destroy a real calendar. The table set itself is
fixed — `CREATE TABLE`, `DROP TABLE` and `ALTER` are all rejected.

## The `calendars` table

Projected from `users/me/calendarList`. Only `summary`, `description`,
`location` and `time_zone` are writable, and only on `INSERT`; `UPDATE` is not
supported.

| Column | Type | Writable | Notes |
| --- | --- | --- | --- |
| `calendar_id` | VARCHAR | no | Google-assigned; the join key to `events.calendar_id` |
| `summary` | VARCHAR | INSERT only | The calendar's name; required on INSERT |
| `description` | VARCHAR | INSERT only | |
| `location` | VARCHAR | INSERT only | |
| `time_zone` | VARCHAR | INSERT only | e.g. `America/New_York` |
| `summary_override` | VARCHAR | no | This user's private renaming of the calendar |
| `access_role` | VARCHAR | no | `owner`/`writer`/`reader`/`freeBusyReader` |
| `color_id` | VARCHAR | no | |
| `background_color` | VARCHAR | no | |
| `foreground_color` | VARCHAR | no | |
| `is_primary` | BOOLEAN | no | Named `is_primary` because `primary` is a SQL keyword |
| `selected` | BOOLEAN | no | Shown in the user's calendar UI |
| `hidden` | BOOLEAN | no | |
| `deleted` | BOOLEAN | no | Deleted entries are skipped when `events` fans out |
| `etag` | VARCHAR | no | |
| `kind` | VARCHAR | no | |

## The `events` table

Fixed columns (a raw-JSON passthrough is the Google JSON for that field,
emitted/accepted as a VARCHAR string):

| Column | Type | Writable | Notes |
| --- | --- | --- | --- |
| `calendar_id` | VARCHAR | INSERT only | Owning calendar's id; not a Google event field. Selects the destination on INSERT, and is half of row identity. Cannot be changed by UPDATE (setting it to its existing value is a no-op, so `UPDATE ... BY NAME` works) |
| `event_id` | VARCHAR | INSERT only | Row identity; the `UPDATE`/`DELETE`/`MERGE` key. Client-suppliable on INSERT (base32hex, len 5–1024), otherwise server-assigned |
| `summary` | VARCHAR | yes | |
| `description` | VARCHAR | yes | |
| `location` | VARCHAR | yes | |
| `status` | VARCHAR | yes | |
| `html_link` | VARCHAR | no | |
| `created` | VARCHAR | no | |
| `updated` | VARCHAR | no | |
| `start` | TIMESTAMP WITH TIME ZONE | yes | |
| `end` | TIMESTAMP WITH TIME ZONE | yes | Important: `end` is a SQL keyword, so must be double-quoted when referenced as an identifier. |
| `all_day` | BOOLEAN | yes | Drives date-vs-dateTime encoding of `start`/`end` |
| `color_id` | VARCHAR | yes | |
| `transparency` | VARCHAR | yes | `opaque` (busy) / `transparent` (free) |
| `visibility` | VARCHAR | yes | `default`/`public`/`private`/`confidential` |
| `event_type` | VARCHAR | yes | |
| `recurring_event_id` | VARCHAR | no | |
| `original_start_time` | TIMESTAMP WITH TIME ZONE | no | |
| `ical_uid` | VARCHAR | no | |
| `sequence` | BIGINT | no | |
| `etag` | VARCHAR | no | |
| `kind` | VARCHAR | no | |
| `hangout_link` | VARCHAR | no | |
| `creator` | VARCHAR | no | raw-JSON passthrough |
| `organizer` | VARCHAR | no | raw-JSON passthrough |
| `end_time_unspecified` | BOOLEAN | no | |
| `attendees_omitted` | BOOLEAN | no | |
| `anyone_can_add_self` | BOOLEAN | yes | |
| `guests_can_invite_others` | BOOLEAN | yes | |
| `guests_can_modify` | BOOLEAN | yes | |
| `guests_can_see_other_guests` | BOOLEAN | yes | |
| `private_copy` | BOOLEAN | no | |
| `locked` | BOOLEAN | no | |
| `attendees` | VARCHAR | yes | raw-JSON passthrough |
| `recurrence` | VARCHAR | yes | raw-JSON passthrough |
| `reminders` | VARCHAR | yes | raw-JSON passthrough |
| `conference_data` | VARCHAR | yes | raw-JSON passthrough |
| `extended_properties` | VARCHAR | yes | raw-JSON passthrough |
| `source` | VARCHAR | yes | raw-JSON passthrough |
| `attachments` | VARCHAR | yes | raw-JSON passthrough |
| `working_location_properties` | VARCHAR | yes | raw-JSON passthrough |
| `out_of_office_properties` | VARCHAR | no | raw-JSON passthrough |
| `focus_time_properties` | VARCHAR | no | raw-JSON passthrough |

## Limitations

- Row identity is `(calendar_id, event_id)`; `UPDATE`/`DELETE`/`MERGE` key on it. It is carried as a
  single VARCHAR `rowid` with the two halves separated by `0x1f`, so a rowid value is only meaningful
  to rows produced by scanning `events`.
- A time bound on `start` is required for reads (unbounded scans error), or set a default window at
  ATTACH with `default_window_length` / `default_window_start` / `default_window_end`.
- `default_calendars` bounds the fan-out for queries that carry no `calendar_id` predicate; it is the
  only lever for statements DuckDB cannot push a predicate into.
- An unqualified `events` scan issues one `events.list` per calendar. Filter on `calendar_id` to
  narrow it. The predicate is pushed down from a `WHERE` clause and from a `MERGE ... ON` clause, but
  **not** past a `WHEN NOT MATCHED BY SOURCE` arm — that requires a full target scan, so such a MERGE
  reads every calendar.
- `UPDATE` is read-modify-write (GET + PUT); `no`-writable columns above are read-only. Moving an
  event between calendars is not supported (that is `events.move`); delete and re-insert instead.
- `calendars` supports `INSERT` (create) and `DELETE` (remove, session-created only); `UPDATE` and
  `MERGE` on it are not supported.
- `RETURNING` is supported; writes are serialized; transient 429/5xx are retried with backoff.
- `test/sql/google_calendar.test` is credential-free and covers the catalog shape and the validation
  errors; `test/sql/live.test` exercises the live API when `GOOGLE_CALENDAR_ACCESS_TOKEN` and
  `GOOGLE_CALENDAR_PRIMARY_ID` are set.
