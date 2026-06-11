variable "project_id" {
  type        = string
  description = "Existing GCP project ID that will own the service account and have the Calendar API enabled."
}

variable "service_account_id" {
  type        = string
  description = "Account ID (the part before '@') for the test service account. 6-30 chars, lowercase letters, digits and hyphens."
  default     = "duckdb-gcal-test"

  validation {
    condition     = can(regex("^[a-z][a-z0-9-]{4,28}[a-z0-9]$", var.service_account_id))
    error_message = "service_account_id must be 6-30 chars: lowercase letters, digits, hyphens; start with a letter."
  }
}
