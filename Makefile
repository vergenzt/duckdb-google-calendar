PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=google_calendar
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ---------------------------------------------------------------------------
# Test infrastructure (Google service account for the key_file provider).
# Prefer OpenTofu if installed, otherwise fall back to terraform.
# ---------------------------------------------------------------------------
TEST_INFRA_DIR := test_infra
TF := $(shell command -v tofu >/dev/null 2>&1 && echo tofu || echo terraform)
KEY_FILE := $(TEST_INFRA_DIR)/sa.json

.PHONY: test-infra-up test-infra-down

# Create the service account and mint a JSON key (via gcloud, so the key never
# enters Terraform state). Idempotent: skips key creation if sa.json exists.
test-infra-up:
	cd $(TEST_INFRA_DIR) && $(TF) init -input=false && $(TF) apply -auto-approve
	@if [ -f $(KEY_FILE) ]; then \
		echo "$(KEY_FILE) already exists; skipping key creation."; \
	else \
		email=$$(cd $(TEST_INFRA_DIR) && $(TF) output -raw service_account_email); \
		echo "Minting key for $$email -> $(KEY_FILE)"; \
		gcloud iam service-accounts keys create $(KEY_FILE) --iam-account=$$email; \
		chmod 600 $(KEY_FILE); \
	fi
	@cd $(TEST_INFRA_DIR) && $(TF) output -raw create_secret_sql; echo

# Delete the local key and destroy the service account (which removes its GCP
# keys). The Calendar API stays enabled (disable_on_destroy = false).
test-infra-down:
	-rm -f $(KEY_FILE)
	cd $(TEST_INFRA_DIR) && $(TF) destroy -auto-approve