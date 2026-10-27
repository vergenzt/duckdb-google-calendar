#!/usr/bin/env bash
# Runs the credential-free example tests. No network access and no secret required: the derivation
# test binds `base_events` to a stub, and the fixture tests drive the extension's mock HTTP seam.
set -euo pipefail
cd "$(dirname "$0")/../.."

DUCKDB=${DUCKDB:-./build/release/duckdb}

export PERSONAL_CALENDAR_ID=personal@x
export EMPLOYER_CALENDAR_ID=employer@x
export CONTRACT_CALENDAR_ID=contract@x
export EMPLOYER_NAME=Employer
export CONTRACT_NAME=Contract
export REPLICA_COLOR_ID=8

echo "== derivations (stub base_events, no API) =="
"$DUCKDB" -init /dev/null -batch -c ".read examples/test/derive_test.sql"

echo "== catalog binding (mock HTTP seam) =="
GOOGLE_CALENDAR_TEST_FIXTURE=examples/test/fixture_empty.json \
  "$DUCKDB" -init /dev/null -batch -c ".read examples/test/fixture_test.sql"

echo "== writes are issued (mock HTTP seam) =="
GOOGLE_CALENDAR_TEST_FIXTURE=examples/test/fixture_one_event.json \
  "$DUCKDB" -init /dev/null -batch -c ".read examples/test/fixture_write_test.sql"
