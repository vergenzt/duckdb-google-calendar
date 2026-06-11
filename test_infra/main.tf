# Enable the Google Calendar API on the project. The test service account calls
# the Calendar API as itself against its own (the SA's) primary calendar, so no
# calendar sharing or project IAM role is required.
resource "google_project_service" "calendar" {
  project = var.project_id
  service = "calendar-json.googleapis.com"

  # Leave the API enabled if this module is destroyed; other things may rely on it.
  disable_on_destroy = false
}

# The identity the DuckDB extension authenticates as. It owns its own Google
# Calendar (cal.primary), which is what the test suite reads from / writes to.
#
# NOTE: the JSON key is deliberately NOT managed here. The `google_service_account_key`
# resource always persists the private key into Terraform/OpenTofu state, which we
# want to avoid. Instead the key is minted out-of-band with `gcloud` (see
# `make test-infra-up`) and written straight to disk, so the secret never enters
# state. Destroying the service account removes any keys it has in GCP.
resource "google_service_account" "test" {
  project      = var.project_id
  account_id   = var.service_account_id
  display_name = "duckdb-google-calendar test account"
  description  = "Service account for testing the duckdb-google-calendar extension (key_file provider)."
}
