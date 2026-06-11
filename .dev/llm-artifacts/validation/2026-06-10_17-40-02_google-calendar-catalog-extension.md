---
template_version: 1
date: 2026-06-10T17:40:02-0400
author: Tim Vergenz
commit: 418af6f
branch: main
repository: duckdb-google-calendar
topic: "Validation of Google Calendar read/write DuckDB catalog extension (ATTACH StorageExtension)"
status: ready
verdict: fail
parent: ".rpiv/artifacts/plans/2026-06-10_14-13-37_google-calendar-catalog-extension.md"
tags: [validation, duckdb-extension, storage-extension, catalog, google-calendar, gsheets-port, dml, filter-pushdown]
last_updated: 2026-06-10T17:40:02-0400
---

## Validation Report: Google Calendar read/write DuckDB catalog extension

### Implementation Status

- ✓ Phase 1: Scaffold rename + build foundation — Fully implemented (all 8 structural checks pass; submodule removed; `json.hpp` vendored; 31-source `EXTENSION_SOURCES`)
- ✓ Phase 2: Transport + retry + mock seam + util (port) — Fully implemented (namespace/include rebind clean; retry decorator + env-var mock seam present)
- ✓ Phase 3: Auth + secret type + client (port + rebind) — Fully implemented (calendar scope rebound; secret type `google_calendar`; 3 providers registered)
- ✓ Phase 4: Calendar types + resources — Fully implemented (calendarList + events CRUD present; no storage forward-refs)
- ✓ Phase 5: StorageExtension + Catalog + Transaction(Manager) — Fully implemented (storage extension registered; 4 `Plan*` overrides; DDL rejected)
- ✓ Phase 6: Schema + table entries + enumeration — Fully implemented (15 columns; `start`/`end` TIMESTAMP_TZ; VARCHAR rowid; collision-safe naming)
- ⚠️ Phase 7: Scan TableFunction (pushdown + pagination) — **Structurally present but functionally broken.** All structural checks pass, but the time-bound extraction never fires: every bounded read fails (see Findings).
- ✓ Phase 8: DML operators (Insert/Update/Delete) — Fully implemented structurally (EXTENSION operators; serial sink; read-modify-write UPDATE). Not exercised at runtime (blocked by the Phase 7 read defect for round-trip verification).
- ✓ Phase 9: MERGE (PlanMergeInto) — Fully implemented structurally (PlanMergeInto declared + defined; reuses 3 DML ops; composes core PhysicalMergeInto).
- ⚠️ Phase 10: Tests + docs — Build passes, credential-free `make test` is green, but `make test` **with** `GOOGLE_CALENDAR_ACCESS_TOKEN` set fails at the live bounded-read (consequence of the Phase 7 defect).

### Automated Verification Results

**Phases 1–9 structural checks — all pass:**

- ✓ Phase 1 (8/8): no `waddle` literals (0), submodule removed, CMake target, `json.hpp`, include dirs, `LOAD_TESTS`, entry macro, 31 `.cpp` sources
- ✓ Phase 2 (5/5): no `namespace sheets` (0), no stale includes (0), `RetryingHttpClient` present, mock env var wired, version macro rebound / no `GSHEETS`
- ✓ Phase 3 (5/5): JWT scope `auth/calendar` (no `spreadsheets`), secret type `google_calendar`, no `gsheet` leak, secret registration wired, 3 providers
- ✓ Phase 4 (5/5): `DoDelete`, `calendar/v3`, `users/me/calendarList`, events CRUD (5), no storage forward-ref (0)
- ✓ Phase 5 (5/5): storage ext registered, 4 `Plan*` overrides, named-secret param, DDL rejected (3), manager dtor
- ✓ Phase 6 (5/5): 15 columns, 2 TIMESTAMP_TZ, VARCHAR rowid, calendarList enumeration, `used_names` (3)
- ✓ Phase 7 (6/6 structural): pushdown+projection, time-bound message, no residual erase (0), pagination (5), `COLUMN_IDENTIFIER_ROW_ID`, `GetScanFunction` wired — **note: these are presence/grep checks; they do not exercise the code path**
- ✓ Phase 8 (5/5): no DML stub (0), EXTENSION operators (3), serial sink, read-modify-write UPDATE, `row_id_index` (3)
- ✓ Phase 9 (5/5): PlanMergeInto decl+def, reuses 3 DML ops, composes PhysicalMergeInto, no concrete coupling (0)

**Phase 10 build/test gate:**

- ✓ Project builds: `make` — completes without error; extension links into DuckDB (`[google_calendar, core_functions, parquet]`)
- ✓ Stale scalar-fn test absent / live test credential-gated / `primary` naming / no `waddle` — all pass
- ✓ Credential-free smoke: `./build/release/test/unittest test/sql/google_calendar.test` — All tests passed (2 assertions)
- ✓ Credential-free live skip: `live.test` correctly skips via `require-env GOOGLE_CALENDAR_ACCESS_TOKEN`
- ✗ **`make test` with `GOOGLE_CALENDAR_ACCESS_TOKEN` set: FAILS** — `test/sql/live.test:16` (the bounded read `SELECT count(*) >= 0 FROM cal.primary WHERE start >= ... AND start < ...`) raises `Binder Error: google_calendar scan requires an explicit time bound on "start"` even though the query supplies the exact bound form the message prescribes. Result: `test cases: 2 | 1 passed | 1 failed`.

### Code Review Findings

#### Matches Plan:

- `CMakeLists.txt` — 31-source `EXTENSION_SOURCES`, OpenSSL on both targets, include dirs, target rename — exactly as specified.
- `src/calendar/**` (transport/auth/util/resources) — faithful port under `duckdb::gcal`; retry decorator and env-var mock seam present per plan.
- `src/storage/calendar_catalog.cpp` — storage-extension registration, 4 `Plan*` DML overrides, `PlanMergeInto` composing `PhysicalMergeInto`, calendarList enumeration with collision-safe naming, `primary` calendar mapping.
- `src/storage/event_schema.cpp` — 15-column schema with `start`/`end` as TIMESTAMP_TZ.
- `test/sql/{google_calendar,live}.test` — credential-free smoke + credential-gated live test, wired as the plan's Phase 10 describes.

#### Deviations from Plan:

- **`src/storage/calendar_scan.cpp` (`ExtractTimeBound`, lines ~84–138; enforced at `CalendarScanInitGlobal`, line ~261) — BLOCKER: the read path is non-functional.** Every bounded-read form fails with the local `BinderError` claiming no time bound is present, despite a valid bound. Reproduced live (ATTACH succeeds, token valid) against `cal.primary` for all of:
  - `SELECT count(*) >= 0 ... WHERE start >= A AND start < B` (the live-test query)
  - `SELECT count(*) ... WHERE start >= A AND start < B`
  - `SELECT id ... WHERE start >= A AND start < B`
  - `SELECT start ... WHERE start >= A AND start < B`
  - `SELECT * ... WHERE start >= A AND start < B`
  - `SELECT count(*) ... WHERE start >= A` (single bound)

  Because even `SELECT *` with a single bound fails, the cause is **not** projection-pushdown stripping the `start` column nor conjunction-splitting — `ExtractTimeBound` simply never sets `has_lower`/`has_upper` for any predicate. `pushdown_complex_filter` *is* invoked (confirmed in `duckdb/src/optimizer/pushdown/pushdown_get.cpp:34`, with top-level ANDs already split into separate conjuncts), so the mismatch is inside `ExtractTimeBound`'s own guards. Most likely suspect: the column-resolution guard `get.names[base_idx] != "start"` (line ~118) — the scan `TableFunction` is registered with a **nullptr bind** (`GetCalendarScanFunction()`: `TableFunction("google_calendar_scan", {}, CalendarScan, nullptr, CalendarScanInitGlobal)`), so whether `LogicalGet::names` / the `col_ids`→`GetPrimaryIndex()`→`get.names` mapping resolves to `"start"` for a catalog-table scan needs verification. The constant-type guard (`value.type().id() != TIMESTAMP_TZ`) is a secondary suspect. This is a localized fix in one file but blocks the entire primary read deliverable.

#### Potential Issues:

- The plan's per-phase Automated Verification for Phase 7 is purely structural (grep for `pushdown_complex_filter`, `next_page_token`, the error-message string). None of these exercise the extraction logic, so the broken read path passed every per-phase gate and only surfaced at the live test. The structural-only verification strategy (inherent to the foundation-first decomposition) has a coverage gap on the scan's filter-extraction behavior.
- DML (Phases 8–9) could not be runtime-verified end-to-end because a working scan is needed to read back / observe round-trips; their correctness rests on structural checks only.

### Manual Testing Required:

1. Read path (after fix):
   - [ ] `SELECT * FROM cal.primary WHERE start >= TIMESTAMPTZ 'A' AND start < TIMESTAMPTZ 'B'` returns rows (issues GET with `timeMin`/`timeMax` + `singleEvents=true&orderBy=startTime`).
   - [ ] `SELECT * FROM cal.primary` (no bound) still errors `requires an explicit time bound`.
   - [ ] Multi-page fixture (`nextPageToken`) returns all rows concatenated.
   - [ ] Scan emits event `id` as the VARCHAR rowid for DELETE/UPDATE paths.
2. DML round-trips (after read path works):
   - [ ] INSERT POSTs one events.insert per row; UPDATE does GET→PUT; DELETE issues events.delete; MERGE performs per-row PUT/POST.
3. `make test` with `GOOGLE_CALENDAR_ACCESS_TOKEN` set is green.

### Recommendations:

- **Do not commit.** Fix `ExtractTimeBound` in `src/storage/calendar_scan.cpp` so a bounded `start` predicate sets `has_lower`/`has_upper`. Start by instrumenting the three early-return guards (column-ref/constant shape at lines ~99–110, the `get.names[base_idx] != "start"` resolution at lines ~112–119, and the `TIMESTAMP_TZ` constant-type check at lines ~123–126) to identify which one rejects a known-good `start >= TIMESTAMPTZ '…'` predicate. The nullptr-bind catalog-scan path makes the `get.names` resolution the prime suspect.
- Re-run the live bounded-read variants above (ATTACH + `SELECT … WHERE start >= A AND start < B`) as the regression check; then re-run `/skill:validate`.
- Consider adding a mock-backed deterministic scan test (the plan defers this to v2) so the filter-extraction path has automated coverage independent of live credentials — this defect would have been caught pre-live by such a test.
- This is a localized code fix, not a plan-structure problem — fix in place and re-validate rather than escalating to `/skill:revise`.
