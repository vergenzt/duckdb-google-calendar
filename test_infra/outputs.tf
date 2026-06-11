output "service_account_email" {
  description = "Email of the test service account. Its primary calendar is what you query as cal.primary; pass this to `gcloud iam service-accounts keys create`."
  value       = google_service_account.test.email
}

output "key_file_path" {
  description = "Conventional path where `make test-infra-up` writes the JSON key, for the extension's key_file provider."
  value       = abspath("${path.module}/sa.json")
}

output "create_secret_sql" {
  description = "Ready-to-paste DuckDB statement to create the credential secret once the key exists on disk."
  value       = "CREATE SECRET cal (TYPE google_calendar, PROVIDER key_file, filepath '${abspath("${path.module}/sa.json")}');"
}
