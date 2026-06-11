# Test credentials via OpenTofu / Terraform

Provisions a throwaway Google **service account** for testing the
`duckdb-google-calendar` extension's `key_file` provider. The extension
authenticates *as* the service account and reads/writes the service account's
**own** Google Calendar (`cal.primary`) — so there is **no calendar to share**
and no manual Console clicking beyond initial auth.

What it manages:

- Enables the Calendar API (`calendar-json.googleapis.com`) on your project.
- A service account (`duckdb-gcal-test@<project>.iam.gserviceaccount.com`).

What it deliberately does **not** manage: the JSON key. The
`google_service_account_key` resource always persists the private key into
Terraform/OpenTofu state, so instead the key is minted out-of-band with
`gcloud` and written straight to `sa.json` (gitignored). The secret never
touches state. `make test-infra-up` does both steps for you.

> Works with OpenTofu (`tofu`) or Terraform 1.5.7. The `make` targets pick
> `tofu` if installed, else `terraform`.

## Prerequisites (one-time)

You already have a GCP project with billing enabled — supply its ID. You need
local credentials that can manage it (Owner/Editor, or at minimum
`roles/serviceusage.serviceUsageAdmin` + `roles/iam.serviceAccountAdmin` +
`roles/iam.serviceAccountKeyAdmin`).

Install the Google Cloud SDK and log in for Application Default Credentials:

```bash
# macOS
brew install --cask google-cloud-sdk      # or: https://cloud.google.com/sdk/docs/install

gcloud auth application-default login      # ADC, used by OpenTofu
gcloud auth login                          # user creds, used by `gcloud ... keys create`
```

Set the project variable:

```bash
cd test_infra
cp terraform.tfvars.example terraform.tfvars   # then edit project_id
```

## Bring it up / down

From the repo root:

```bash
make test-infra-up      # apply + mint sa.json + print the CREATE SECRET statement
make test-infra-down    # delete sa.json + destroy the service account
```

`test-infra-up` is idempotent: if `sa.json` already exists it won't mint another
key (GCP caps a service account at 10 keys).

## Use it with the extension

```sql
CREATE SECRET cal (TYPE google_calendar, PROVIDER key_file, filepath '/abs/path/test_infra/sa.json');
ATTACH 'me' AS cal (TYPE google_calendar, SECRET cal);

-- cal.primary is the SERVICE ACCOUNT's own calendar (starts empty):
INSERT INTO cal.primary (summary, start, "end")
VALUES ('Smoke test', TIMESTAMPTZ '2026-06-10 09:00-04', TIMESTAMPTZ '2026-06-10 09:15-04');

SELECT id, summary, start, "end" FROM cal.primary
WHERE start >= TIMESTAMPTZ '2026-06-01 00:00-04'
  AND start <  TIMESTAMPTZ '2026-07-01 00:00-04';
```

## Storing the key in 1Password (no file on disk)

The `key_file` provider also accepts `key_env`, which names an environment
variable holding the **full key JSON**. The secret never lands on disk and never
appears in the SQL statement (or query history / CI logs) — only the variable
name does. This is the preferred flow for 1Password + CI.

Upload the minted key to 1Password once, then delete the local copy:

```bash
op document create test_infra/sa.json --title "duckdb-gcal test SA key" --vault Private
rm test_infra/sa.json
```

Local testing with `op run` — inject the document as an env var, no temp file:

```bash
# .env.op  (committed; contains only references, not secrets)
GCAL_SA_KEY=op://Private/duckdb-gcal test SA key/document

op run --env-file=.env.op -- make test          # GCAL_SA_KEY is set for the run
```

```sql
CREATE SECRET cal (TYPE google_calendar, PROVIDER key_file, key_env 'GCAL_SA_KEY');
```

In CI, store the JSON as a GitHub Actions secret (e.g. `GCAL_SA_KEY`) and expose
it as an env var — again, nothing touches disk:

```yaml
- run: make test
  env:
    GCAL_SA_KEY: ${{ secrets.GCAL_SA_KEY }}
```

(If you'd rather use a file, `op document get ... --out-file test_infra/sa.json`
plus the `filepath` parameter still works; it's gitignored.)

## Notes

- Destroying the service account (`make test-infra-down`) removes all its keys
  in GCP; the Calendar API stays enabled.
- The `access_token` provider (OAuth Playground token) remains the fastest path
  for a one-off read; this module is for durable, non-expiring test credentials.
