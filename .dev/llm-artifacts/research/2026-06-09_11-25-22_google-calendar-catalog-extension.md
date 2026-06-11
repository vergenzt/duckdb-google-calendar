---
date: 2026-06-09T11:25:22-0400
author: Tim Vergenz
commit: 418af6f
branch: main
repository: duckdb-google-calendar
topic: "Google Calendar read/write DuckDB extension (ATTACH catalog)"
tags: [research, codebase, duckdb-extension, storage-extension, catalog, google-calendar, gsheets-port]
status: ready
last_updated: 2026-06-09T11:25:22-0400
last_updated_by: Tim Vergenz
---

# Research: Google Calendar read/write DuckDB extension (ATTACH catalog)

## Research Question
How do we build a DuckDB `StorageExtension` registered for `TYPE google_calendar` that, on `ATTACH`, enumerates calendars via the Calendar API `calendarList` and builds a catalog with one `events` table per calendar — backed by custom physical scan + insert/update/delete operators that translate to `events.list/insert/patch/delete` — by porting the layered transport/resource/auth client subtree from `reference/duckdb_gsheets/src/sheets/` and using a `google_calendar` Secret type for credentials?

## Summary

The feature splits cleanly into **net-new DuckDB catalog work** (no analog in the gsheets reference) and a **directly portable client stack** (auth, HTTP transport, JSON, secret resolution).

- **Net-new (build from scratch against the `duckdb/` submodule headers):** a `StorageExtension` subclass, a custom `Catalog` (~13 pure-virtuals), `SchemaCatalogEntry` (~15 pure-virtuals), `TableCatalogEntry` (3 pure-virtuals), a `TransactionManager`, a scan `TableFunction` with filter pushdown, and four DML physical operators (`Insert`/`Update`/`Delete` + `PlanMergeInto`). The **SQLite scanner (`duckdb-sqlite`) is the cleanest template** — string/simple rowid, near-minimal override set; the **Postgres scanner (`duckdb-postgres`) shows the full surface** including `PlanMergeInto`.
- **Portable (clone + rebind literals):** the `gsheet` Secret type → `google_calendar`, the `IHttpClient`/`BaseResource`/facade layering, RS256 JWT service-account signing, OAuth flow, and the nlohmann JSON `ParseResponse<T>` idiom. Exactly **two** Sheets scope literals plus the OAuth `client_id`/`redirect_uri` must change; the API base URL must change to `https://www.googleapis.com/calendar/v3`.
- **Gaps the reference does not cover and that must be built:** (1) the entire catalog/DML layer; (2) 429/5xx exponential-backoff retry (absent everywhere); (3) a sqllogictest-level mock-HTTP injection seam (absent — every reference SQL test hits the live API behind `require-env`); (4) a query-string builder + `nextPageToken` pagination; (5) a `DoDelete` helper (transport supports `DEL`, but `BaseResource` exposes no delete helper).

**Critical API subtlety:** `events.list` `timeMin`/`timeMax` are **both exclusive** and **asymmetric** — `timeMin` filters on the event's **end** time, `timeMax` on the event's **start** time (half-open overlap). SQL predicates on the `start` column therefore map only **approximately** to the API window; the extension must over-fetch and re-apply the exact predicate (favor `pushdown_complex_filter` so DuckDB keeps the residual filter).

**Resolved decisions (this session):** UPDATE → `events.update` (PUT) with **read-modify-write** (GET, merge changed columns, PUT) — no PATCH needed; mock injection → **factory branch in `CreateHttpClient` on a test setting/env var**; JSON dependency → **vendor `json.hpp`** (mirror gsheets, no vcpkg change).

## Detailed Findings

### Catalog backbone — `ATTACH` → custom `Catalog` (net-new)

- Registration replaces the two `loader.RegisterFunction` calls (`src/waddle_extension.cpp:29-43`) with a single `StorageExtension::Register(config, "google_calendar", make_shared_ptr<...>())` (impl at `duckdb/src/main/extension_callback_manager.cpp:163-166`). It takes `DBConfig &` (reach it from the loader's database instance). The `TYPE google_calendar` token matches the registration key (case-insensitive, via `ExtensionHelper::ApplyExtensionAlias`).
- The `StorageExtension` subclass assigns two `std::function` members in its ctor: `attach` (`attach_function_t`) and `create_transaction_manager` (`create_transaction_manager_t`) — typedefs at `duckdb/src/include/duckdb/storage/storage_extension.hpp:25-30`. Minimal in-tree template: `duckdb/test/api/test_storage_extension_alias.cpp:11-22`.
- Dispatch on `ATTACH`: `DatabaseInstance::CreateAttachedDatabase` (`duckdb/src/main/database.cpp:168-194`) → storage-extension `AttachedDatabase` ctor (`duckdb/src/main/attached_database.cpp:133-162`); the attach callback runs at `attached_database.cpp:146`, the transaction manager at `:155`. Because a custom catalog returns `IsDuckCatalog() == false`, **no on-disk `SingleFileStorageManager` is created** (`attached_database.cpp:151-153`) — correct for an API-backed catalog.
- **Secret name flows through options:** `AttachInfo.options` (`duckdb/src/include/duckdb/parser/parsed_data/attach_info.hpp:19-46`) and `AttachOptions.options` (`duckdb/src/include/duckdb/main/attached_database.hpp:60-80`). `TYPE` becomes `db_type` (`attached_database.cpp:70-73`); every other key (e.g. `SECRET 'x'`) falls through to `options.emplace(...)` at `attached_database.cpp:90`. Read it via the gsheets `GetStringOption` idiom (`reference/duckdb_gsheets/src/include/utils/options.hpp:13-14`).
- **Catalog pure-virtuals to implement** (checklist from `duckdb/src/include/duckdb/catalog/duck_catalog.hpp:40-78`): `Initialize` (`catalog.hpp:116`), `GetCatalogType` (`:134`), `CreateSchema` (`:139` — throw), `LookupSchema` (`:222`), `ScanSchemas` (`:253`), `PlanCreateTableAs` (`:308`), `PlanInsert` (`:310`), `PlanDelete` 4-arg (`:312`), `PlanUpdate` 4-arg (`:315`), `GetDatabaseSize` (`:327`), `InMemory` (`:330`), `GetDBPath` (`:331`), `DropSchema` (`:452` — throw). `PlanMergeInto` (`:318`) is **non-pure but defaults to throwing** (`plan_merge_into.cpp:120-122`) — must override for MERGE. Web research flags `BindCreateIndex` as also pure on current `main`.
- Pair it with a `TransactionManager`. For a non-transactional REST backend `StartTransaction`/`Commit`/`Rollback` are near-no-ops (model: `duckdb-sqlite` `sqlite_transaction_manager.cpp`, ~40 lines; the in-tree test reuses `DuckTransactionManager`). A `Transaction` subclass carries the per-statement HTTP client/auth and exposes a `static Get(ClientContext&, Catalog&)` the operators call.

### Dynamic per-calendar enumeration (net-new)

- The attach callback resolves the secret (see Auth), builds an HTTP client + auth headers, then calls a new `CalendarListResource` (model: `SpreadsheetResource` at `reference/duckdb_gsheets/src/sheets/resources/spreadsheet.cpp:15-18` over `BaseResource::DoGet` at `resources/base.cpp:7-13`) against `GET /users/me/calendarList`, deserializing with `ParseResponse<T>` (`reference/duckdb_gsheets/src/include/sheets/util/response.hpp:11-21`) into a `CalendarListResponse` struct using `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` (model `types.hpp:43-48`).
- One `TableCatalogEntry` subclass per calendar, all under a single `SchemaCatalogEntry`, built from a `CreateTableInfo` (`duckdb/src/include/duckdb/parser/parsed_data/create_table_info.hpp:19-36`) whose `columns` carry the fixed `events` schema: `id`/`summary`/`description`/`location`/`status`/`html_link`/`created`/`updated` → `VARCHAR`; `start`/`end` → `LogicalType::TIMESTAMP_TZ` (`duckdb/src/include/duckdb/common/types.hpp:412`); `all_day` → `BOOLEAN`; `attendees`/`recurrence`/`reminders`/`conference_data` → `JSON()` (or `VARCHAR` to avoid a hard json-extension dependency).
- **Name-collision handling is the extension's job** — entries are keyed by unique name in a `CatalogSet`; two calendars with the same `summary` collide. Derive a unique table name (slug of `summary`, suffix with counter/`id` on collision) in the enumeration loop. Use the calendar `id` (email-style, e.g. `...@group.calendar.google.com`, or `primary`) as the stable key for all subsequent API calls — `summary` is a read-only display label, not interchangeable with `id`.

### Read path + predicate pushdown (net-new)

- gsheets is a **table function + replacement scan** (`gsheets_extension.cpp:55,68`), fetches everything eagerly **in bind** (`gsheets_read.cpp:189`), caches in `ReadSheetBindData` (`include/gsheets_read.hpp:10-20`), and never wires filter pushdown. The chunked-emission cursor idiom (`finished`/`row_index`/`SetCardinality`, `gsheets_read.cpp:31-84`) is reusable; the eager-bind fetch and `mutable`-in-bind-data cursor are **not** — move fetch to `init_global` and hold cursor/page state in a `GlobalTableFunctionState`.
- A catalog table exposes its scan via `TableCatalogEntry::GetScanFunction` (`duckdb/src/include/duckdb/catalog/catalog_entry/table_catalog_entry.hpp:101`), called by the binder at `duckdb/src/planner/binder/tableref/bind_basetableref.cpp:236`; column types come from catalog columns (`bind_basetableref.cpp:246-252`), **not** a bind callback (so the `events` schema is fixed). Set `filter_pushdown = true` on the returned `TableFunction` (flag in `duckdb/src/include/duckdb/function/table_function.hpp`).
- **Ordering constraint:** filters are produced by the optimizer (`duckdb/src/optimizer/pushdown/pushdown_get.cpp:58`) *after* bind. They arrive at `init_global` via `TableFunctionInitInput::filters` (`table_function.hpp:138`; model `duckdb/src/function/table/table_scan.cpp:663-674`). The **"explicit time bound required" error must be raised in `init_global`** (or a `pushdown_complex_filter` callback) — strictly before the first `DoGet`, but not in the gsheets-style bind body.
- `start >= A AND start < B` arrives as **one** `TableFilterSet` entry (`duckdb/src/include/duckdb/planner/table_filter.hpp:84-87`) for the `start` column whose value is a `ConjunctionAndFilter` of two `ConstantFilter`s (merged at `duckdb/src/planner/table_filter.cpp:17-25`). Pushable comparisons: `=, >, >=, <, <=, !=` (`duckdb/src/optimizer/filter_combiner.cpp:273-284`); `BETWEEN` decomposes to `>= AND <=`; timestamp `IN` becomes an advisory `OPTIONAL_FILTER` (not a clean bound). Operands are normalized column-relative (`filter_combiner.cpp:725-746`).
- **API mapping is approximate** (timeMin exclusive-on-end, timeMax exclusive-on-start): derive the widest API window guaranteed to contain all matches, then keep the exact predicate as a residual. With `filter_pushdown = true` DuckDB does **not** re-check pushed filters — so either evaluate the residual in the scan, or use `pushdown_complex_filter` (`pushdown_get.cpp:18-46`) which lets DuckDB retain a `LogicalFilter`. The complex-filter route is the clean fit for "extract a hint, let DuckDB enforce exactness."
- Net-new: a URL-encoded query-string builder (`timeMin`/`timeMax`/`pageToken`/`singleEvents=true`/`orderBy=startTime`/`maxResults`) and a `nextPageToken` loop — gsheets has neither (single-request `ValuesResource::Get`, `values.cpp:11-14`).

### DML operators (net-new)

- All DML routes through `Catalog::Plan*` virtuals (`catalog.hpp:308-319`). Each returns a custom `PhysicalOperator` (type `EXTENSION`) implementing the **Sink** interface (`Sink`/`Combine`/`Finalize`/`GetGlobalSinkState`/`GetLocalSinkState`, `duckdb/src/include/duckdb/execution/physical_operator.hpp:174-193`) plus a **Source** side (`GetData`, `:128`) to emit the affected-row count. Recommend `ParallelSink() == false` to serialize API calls.
- **INSERT:** dispatch `plan_insert.cpp:147` → override `PlanInsert`; `Sink` loops chunk rows (like the gsheets bulk loop `gsheets_copy.cpp:159-173`) but issues **one** `events.insert` POST per row via a new `EventsResource::Insert` over `BaseResource::DoPost` (`base.cpp:14-21`); server `id` ignored on insert. `LogicalInsert.column_index_map`/`expected_types` (`logical_insert.hpp:53-59`) drive a defaults projection. The gsheets `GSheetWriteSink` (a `CopyFunction`, `gsheets_copy.cpp:26,124-179`) is **not reusable** — incompatible signature, bulk single-`ValueRange` append, no row identity.
- **DELETE:** dispatch `plan_delete.cpp:29` → override 4-arg `PlanDelete` (`catalog.hpp:312`); rowid arrives as `op.expressions[0]` (`BoundReferenceExpression`, `plan_delete.cpp:12-15`); `Sink` reads it and issues `events.delete` (DELETE). Transport supports `DEL` (`http_client.cpp:28-30`, `httplib_client.cpp:66-67`) but `BaseResource` has **no `DoDelete` helper** (`base.hpp:19-21`) — add one.
- **UPDATE → `events.update` (PUT) with read-modify-write (resolved):** dispatch `plan_update.cpp:28` → override 4-arg `PlanUpdate` (`catalog.hpp:315`). `op.columns`/`op.expressions` carry SET targets/values (`logical_update.hpp:31`); rowid is appended to the projection (`bind_update.cpp:194`). For each row: GET the full event, merge the changed columns, PUT the whole resource via `DoPut` (`base.cpp:23-30`). **No PATCH support is added** — `HttpMethod` stays `{GET,POST,PUT,DEL}` (`http_type.hpp:11`). (Trade-off accepted: one extra GET per updated row; avoids the full-replace clobber that a blind PUT would cause and the transport work that PATCH would require.)
- **Rowid = server `id`:** override `TableCatalogEntry::GetRowIdColumns`/`GetVirtualColumns` (defaults at `table_catalog_entry.cpp:374-384`) so the virtual "rowid" column is the event `id` (`VARCHAR`). `BindRowIdColumns` (`bind_update.cpp:87-115`) builds the `BoundColumnRefExpression` from that type and adds the column to the `LogicalGet` (`bind_update.cpp:112-114`); the scan must emit `id` when requested. SQLite's simple-rowid model (`duckdb-sqlite` `sqlite_delete.cpp`) is the template; Postgres's packed `ctid` is not relevant.
- **MERGE:** override `PlanMergeInto` (`catalog.hpp:318`) mirroring `DuckCatalog::PlanMergeInto` (`plan_merge_into.cpp:91-118`) — per `BoundMergeIntoAction`, build synthetic `Logical{Insert,Update,Delete}` and delegate to the extension's own `Plan*`, composing into a core `PhysicalMergeInto` with `op.row_id_start`/`source_marker`. `ON id` joins on the same rowid.

### Auth + Secret type (portable; rebind literals)

- Clone `CreateGsheetSecretFunctions::Register` (`gsheets_auth.cpp:108-138`): `SecretType` with `KeyValueSecret::Deserialize` (portable), `default_provider = "oauth"`, and three providers `access_token`/`oauth`/`key_file`. `key_file` parses `client_email`/`private_key` from service-account JSON (`gsheets_auth.cpp:88-89`) — API-agnostic, portable verbatim.
- Resolution: `CreateAuthFromSecret` (`auth_factory.cpp:10-34`) → `GetSecretMatch(ctx, path, type)` (`utils/secret.cpp:9-13`) → dispatch `ServiceAccountAuth` vs `BearerTokenAuth`. RS256 JWT signing (`service_account_auth.cpp:21-107`, OpenSSL) and token exchange are portable.
- **Must change:** secret type string `"gsheet"` → `"google_calendar"` (`gsheets_auth.cpp:109`); the `GetSecretMatch` type arg (`auth_factory.cpp:11`); **two** scope literals `.../auth/spreadsheets` → `.../auth/calendar` (`service_account_auth.cpp:60`, `gsheets_auth.cpp:150`); OAuth `client_id` (`gsheets_auth.cpp:142`) and `redirect_uri` (`gsheets_auth.cpp:143`) → Calendar-enabled values; API base `DEFAULT_SHEETS_API_URL` (`client.hpp:12`) → `https://www.googleapis.com/calendar/v3`. Bearer token applied in `BuildHeaders` (`client.hpp:30-39`). Full read/write scope is `https://www.googleapis.com/auth/calendar`.

### Transport + retry + tests (port + build)

- **Single chokepoint:** every HTTP call funnels through `IHttpClient::Execute` (`http_client.hpp:11`); the `Get/Post/Put/Delete` helpers route through it (`http_client.cpp:22-31`). `HttpLibClient::Execute` returns the status **without throwing** on non-2xx (`httplib_client.cpp:71-77` — throws only on transport failure); non-200 becomes an exception later in `ParseResponse` (`response.hpp:13-14`) and in the OAuth exchange (`service_account_auth.cpp:120`).
- **Retry (build — absent in reference):** add a `RetryingHttpClient : IHttpClient` decorator overriding only `Execute`, inspecting `HttpResponse.statusCode` (`http_type.hpp:22`) and retrying 429/5xx (and 403 `rateLimitExceeded`/`userRateLimitExceeded`) with exponential backoff + jitter. Wrap the real client in `CreateHttpClient` (`client_factory.cpp:12`). This seam covers all verbs **and** the direct token-exchange POST that bypasses `BaseResource`. Make the sleep injectable/zeroable for tests.
- **Mock injection (build — resolved: factory branch):** `MockHttpClient` is a queue + recorder (`mock_http_client.cpp:7-21`); unit tests inject it by constructor reference (`test_client.cpp:18`, `test_values.cpp:18`). There is **no SQL-level seam** — `CreateHttpClient` hard-wires `HttpLibClient`. **Add a branch in `CreateHttpClient` (`client_factory.cpp:11`)** that returns a `MockHttpClient` (optionally wrapped in `RetryingHttpClient`) seeded from a fixture when a test-only DuckDB setting or env var is set — enabling credential-free `make test` and `429,429,200` retry assertions via `GetRecordedRequests().size() == 3`.
- **Test tiers:** sqllogictest (`test/sql/*.test`; `require`, `statement error`, `query I`) for the deterministic mock-backed path; credential-gated live tests use `require-env` (`reference/duckdb_gsheets/test/sql/read_gsheet.test:5`) so they skip in CI. Optional Catch2 C++ unit harness (`reference/duckdb_gsheets/test/unit/` + `stubs/duckdb_stubs.cpp`) for serialization/retry unit tests.

### Build + rename (port + rename)

- **Vendor `json.hpp` (resolved):** copy `reference/duckdb_gsheets/third_party/json.hpp` + add `include_directories(third_party)` — **no vcpkg change** (gsheets `vcpkg.json` is also openssl-only). httplib is vendored via the submodule: `include_directories(duckdb/third_party/httplib)` (present at `duckdb/third_party/httplib/httplib.hpp`).
- Expand `EXTENSION_SOURCES` (`CMakeLists.txt:21`) from the single file to the ported subtree (model list `reference/duckdb_gsheets/CMakeLists.txt:16-38`). Keep OpenSSL `find_package`/link (`CMakeLists.txt:9,25-26`).
- Rename every `waddle` literal: `CMakeLists.txt:4,21`; `extension_config.cmake:4` (+ add `LOAD_TESTS`); `Makefile:4`; `src/waddle_extension.{cpp,hpp}` (class `WaddleExtension`, `Name()`/`Version()`, `EXT_VERSION_WADDLE`, entry macro `DUCKDB_CPP_EXTENSION_ENTRY(waddle,...)` at `:61`, file renames); `.github/workflows/MainDistributionPipeline.yml:21,29`; `test/sql/waddle.test`. The template ships `scripts/bootstrap-template.py` (the rename helper, not yet run) — prefer it over hand-editing to keep all sites in sync.

## Code References
- `src/waddle_extension.cpp:29-43` — `LoadInternal`; swap `RegisterFunction` calls for `StorageExtension::Register`
- `src/waddle_extension.cpp:61` — `DUCKDB_CPP_EXTENSION_ENTRY(waddle, loader)` entry macro to rename
- `CMakeLists.txt:4,21` — `TARGET_NAME`, single-file `EXTENSION_SOURCES` to expand
- `extension_config.cmake:4` — `duckdb_extension_load(waddle ...)`; add `LOAD_TESTS`
- `vcpkg.json:2-4` — deps (openssl only; keep, vendor json)
- `duckdb/src/include/duckdb/storage/storage_extension.hpp:25-50` — `attach_function_t`/`create_transaction_manager_t`/`Register`
- `duckdb/src/main/attached_database.cpp:133-162` — attach + transaction-manager invocation on ATTACH
- `duckdb/src/include/duckdb/catalog/catalog.hpp:116-452` — Catalog pure-virtuals (`Initialize`, `LookupSchema:222`, `ScanSchemas:253`, `PlanInsert:310`, `PlanDelete:312`, `PlanUpdate:315`, `PlanMergeInto:318`)
- `duckdb/src/include/duckdb/catalog/duck_catalog.hpp:40-78` — override checklist
- `duckdb/test/api/test_storage_extension_alias.cpp:11-22` — minimal StorageExtension template
- `duckdb/src/include/duckdb/catalog/catalog_entry/schema_catalog_entry.hpp:54-108` — Schema pure-virtuals
- `duckdb/src/include/duckdb/catalog/catalog_entry/table_catalog_entry.hpp:89,101,118,129,131` — `GetStatistics`/`GetScanFunction`/`GetStorageInfo`/`GetRowIdColumns`/`GetVirtualColumns`
- `duckdb/src/planner/binder/tableref/bind_basetableref.cpp:236-263` — binder calls `GetScanFunction`, builds `LogicalGet`
- `duckdb/src/include/duckdb/function/table_function.hpp:138` — `TableFunctionInitInput::filters`
- `duckdb/src/function/table/table_scan.cpp:663-674` — `init_global` reading `input.filters` (model)
- `duckdb/src/optimizer/pushdown/pushdown_get.cpp:18-58` — filter pushdown into the scan
- `duckdb/src/planner/table_filter.cpp:17-25` — two predicates merged into `ConjunctionAndFilter`
- `duckdb/src/optimizer/filter_combiner.cpp:273-284,725-746` — pushable comparisons, operand normalization
- `duckdb/src/execution/physical_plan/plan_insert.cpp:138-147` / `plan_delete.cpp:10-29` / `plan_update.cpp:9-28` / `plan_merge_into.cpp:91-122` — Plan* dispatch + DuckCatalog reference impls
- `duckdb/src/include/duckdb/execution/physical_operator.hpp:124-210` — Sink/Source interface
- `duckdb/src/planner/binder/statement/bind_update.cpp:87-115,194` — `BindRowIdColumns`
- `reference/duckdb_gsheets/src/gsheets_auth.cpp:108-187` — secret registration + OAuth flow (scope `:150`, client_id `:142`, redirect `:143`)
- `reference/duckdb_gsheets/src/sheets/auth_factory.cpp:10-34` — secret → provider dispatch (type literal `:11`)
- `reference/duckdb_gsheets/src/sheets/auth/service_account_auth.cpp:21-120` — RS256 JWT (scope `:60`)
- `reference/duckdb_gsheets/src/sheets/resources/base.cpp:7-30` — `DoGet`/`DoPost`/`DoPut` (no `DoDelete`)
- `reference/duckdb_gsheets/src/sheets/resources/spreadsheet.cpp:15-18,70-74` — resource Get/BatchUpdate idiom
- `reference/duckdb_gsheets/src/include/sheets/util/response.hpp:11-21` — `ParseResponse<T>`
- `reference/duckdb_gsheets/src/sheets/transport/http_client.hpp:11-21` — `IHttpClient` contract
- `reference/duckdb_gsheets/src/sheets/transport/http_type.hpp:10-25` — `HttpMethod`/`HttpRequest`/`HttpResponse`
- `reference/duckdb_gsheets/src/sheets/transport/httplib_client.cpp:29-82` — real client (status not thrown `:77`; no retry)
- `reference/duckdb_gsheets/src/sheets/transport/client_factory.cpp:11-14` — `CreateHttpClient` (mock-seam insertion point)
- `reference/duckdb_gsheets/src/sheets/transport/mock_http_client.cpp:7-21` — queue + recorder
- `reference/duckdb_gsheets/src/gsheets_read.cpp:31-84,189-194` — chunked-emission cursor + eager-bind fetch
- `reference/duckdb_gsheets/src/gsheets_copy.cpp:26,124-179` — bulk COPY sink (not reusable for DML)
- `reference/duckdb_gsheets/CMakeLists.txt:11-46` — `include_directories`/`EXTENSION_SOURCES`/build idioms to mirror
- `reference/duckdb_gsheets/third_party/json.hpp` — single-header json to vendor

## Integration Points

### Inbound References
- `ATTACH '<account>' AS cal (TYPE google_calendar)` → `DatabaseInstance::CreateAttachedDatabase` (`duckdb/src/main/database.cpp:168-194`) → attach callback (`attached_database.cpp:146`)
- `SELECT ... FROM cal."<calendar>"` → binder `bind_basetableref.cpp:236` → `TableCatalogEntry::GetScanFunction`
- `INSERT/UPDATE/DELETE/MERGE` → `plan_insert.cpp:147` / `plan_delete.cpp:29` / `plan_update.cpp:28` / `plan_merge_into.cpp:128` → Catalog `Plan*` overrides
- Optimizer filter pushdown → `pushdown_get.cpp:58` → `TableFunctionInitInput::filters` at `init_global`

### Outbound Dependencies
- Calendar API `GET /users/me/calendarList` (attach-time enumeration)
- Calendar API `GET /calendars/{id}/events` (scan; `timeMin`/`timeMax`/`pageToken`/`singleEvents`/`orderBy`)
- Calendar API `POST/PUT/DELETE /calendars/{id}/events[/{eventId}]` (insert/update/delete)
- OAuth token endpoint `https://oauth2.googleapis.com/token` (service-account JWT exchange)
- DuckDB `SecretManager::LookupSecret` (credential resolution)
- Vendored `httplib.hpp` (`duckdb/third_party/httplib`), `json.hpp` (`third_party/`), OpenSSL (JWT signing)

### Infrastructure Wiring
- `StorageExtension::Register(config, "google_calendar", ...)` in `LoadInternal` (DI into `DBConfig::storage_extensions`)
- `loader.RegisterSecretType` + 3 `CreateSecretFunction`s (`gsheets_auth.cpp:134-137` pattern)
- `extension_config.cmake` `duckdb_extension_load(... LOAD_TESTS)` registers `test/sql/*.test`
- `CreateHttpClient` factory branch (new) for mock injection under tests
- `.github/workflows/MainDistributionPipeline.yml:21,29` — CI `extension_name`

## Architecture Insights
- **Two-track build:** the client subtree (auth/transport/resource/JSON) is a near-verbatim port with literal rebinding; the catalog/DML layer is entirely net-new against the `duckdb/` submodule headers. Budget accordingly — the hard, novel work is the catalog, not the HTTP plumbing.
- **SQLite scanner is the reference template, not gsheets**, for the catalog/DML shape: single schema built in `Initialize`, one table entry per backend object, simple (here: string) rowid, per-row Sink operators, no-op transactions. Postgres scanner shows the full surface including `PlanMergeInto` and the synthetic-Logical*-delegation pattern.
- **DuckDB version is recent** — the pinned submodule exposes `LookupSchema` (not legacy `GetSchema`), reference-based `Plan*` returning `PhysicalOperator &` with `optional_ptr<PhysicalOperator>` plan args, and `ExtensionLoader`. The codebase agents read the actual pinned headers, so the signatures in Code References are authoritative for this checkout (web-sourced scanner signatures are corroborating, not primary).
- **Single `Execute` chokepoint** makes retry a clean decorator and makes the mock a drop-in — both hinge on `HttpLibClient` returning (not throwing) the HTTP status.
- **API/SQL impedance mismatch on time:** the `start` column is a single instant, but `events.list` filters by half-open overlap with exclusive, asymmetric bounds. Treat pushdown as a fetch-narrowing hint and let DuckDB enforce the exact predicate.

## Precedents & Lessons
1 relevant precedent analyzed (history is shallow — fresh scaffold).

### Precedent: Initial scaffold (the only commit)
**Commit(s)**: `418af6f` — "Initial commit" (2026-06-08)
**Blast radius**: 23 files, 994 insertions — verbatim DuckDB C++ extension template named `waddle`
  src/ — scalar-function template entry point
  build — `CMakeLists.txt`, `extension_config.cmake`, `Makefile`, `vcpkg.json` (openssl only)
  .github/ — CI workflows still named for the template
  test/ — `test/sql/waddle.test`

**Follow-up fixes**: None — single commit, no history.

**Lessons from docs**:
- `.rpiv/artifacts/discover/2026-06-09_10-56-24_google-calendar-catalog-extension.md` — the FRD flags the rename, the missing json dependency, and that the StorageExtension/catalog layer has no reference to copy.

**Takeaway**: Every rename is happening for the first time — use `scripts/bootstrap-template.py` to keep source/build/test/CI in sync; the catalog/DML path is net-new with zero precedent.

### Composite Lessons
- **The rename touches every layer at once** (source, build, tests, CI) — a missed site (CI workflow name or `extension_config.cmake` target) is the classic template-rename breakage (`commit 418af6f`).
- **Add dependencies before porting** — the ported gsheets sources won't compile until json/httplib include paths are wired (here: vendor + `include_directories`).
- **Commit the submodule + `.gitmodules` together** — `reference/duckdb_gsheets` is a staged-but-uncommitted submodule (gitlink `a209d8a`); landing one without the other leaves a broken clone.
- **No precedent for catalog/DML** — auth/HTTP/JSON port cleanly; `StorageExtension` + per-row operators are net-new, not a port.
- **Decide the submodule's fate before release** — a `reference/` submodule ships nothing; confirm it stays dev-only or is removed so it doesn't leak into the distributed extension.

## Historical Context (from `.rpiv/artifacts/`)
- `.rpiv/artifacts/discover/2026-06-09_10-56-24_google-calendar-catalog-extension.md` — the FRD: intent, goals, decisions (catalog write model, table-per-calendar, server-id identity, require-time-bound, fail-fast+retry), and open questions feeding this research.

## Developer Context
**Q (discover: Primary user & intent): What's the core problem this extension solves, and who is the primary user?**
A: Two-way sync / automation author — scripting calendar mutations from SQL where read and write are equally central.

**Q (discover: Auth via DuckDB Secret Manager): Which auth model?**
A: New `google_calendar` secret type mirroring gsheets' oauth / access_token / key_file providers (confirmed).

**Q (discover: Reuse gsheets HTTP/JSON stack): Which client stack?**
A: Vendored cpp-httplib + OpenSSL behind `IHttpClient`, nlohmann::json, layered transport/resource/auth subtree (confirmed).

**Q (discover: Rename scaffold off 'waddle'): Rename the template?**
A: Replace the `waddle` scalar-function template (target name, class, entry macro, test) with the calendar extension (confirmed).

**Q (discover: Write mechanism — StorageExtension catalog): How should writes work?**
A: ATTACH + StorageExtension catalog with native INSERT/UPDATE/DELETE (+ MERGE via planner).

**Q (discover: ATTACH model — table per calendar): What schema does ATTACH produce?**
A: Account → one table per calendar (enables multi-calendar joins; accepts a `calendarList` call at attach time + dynamic enumeration + collision handling).

**Q (discover: Event identity — server id): How are rows identified?**
A: Server `id` is the key; MERGE via standard SQL `ON` (no client iCalUID import in v1).

**Q (discover: Event schema): How rich is the events table?**
A: Core scalar columns plus attendees/recurrence/reminders/conference_data as JSON passthrough.

**Q (discover: Datetime model): How are start/end represented?**
A: `start`/`end` as TIMESTAMP WITH TIME ZONE; `all_day` BOOLEAN for date-only events.

**Q (discover: Read strategy): How does SELECT handle volume?**
A: Push time predicates to timeMin/timeMax + paginate, BUT require an explicit time bound — error if absent.

**Q (discover: Write failure handling): Partial-failure / rate-limit behavior?**
A: Fail fast on hard errors (no rollback) + transparent retry with exponential backoff on transient 429/5xx.

**Q (discover: Testing approach): What defines 'done' for v1?**
A: Mock HTTP client + sqllogictests AND credential-gated live integration tests.

**Q (`http_type.hpp:11`, `base.cpp:7-30`): UPDATE has no PATCH in the ported transport, and PUT full-replaces. How should SQL UPDATE map to the API?**
A: `events.update` (PUT) with **read-modify-write** — GET the full event, merge changed columns, PUT the whole resource. No PATCH added; accepts one extra GET per updated row.

**Q (`client_factory.cpp:12`): No mock-HTTP injection seam exists for SQL-level tests. Which mechanism?**
A: **Factory branch in `CreateHttpClient`** on a test-only setting/env var, returning a `MockHttpClient` seeded from a fixture (optionally wrapped in the retry decorator).

**Q (`vcpkg.json:2-4`, `reference/duckdb_gsheets/third_party/json.hpp`): FRD says add nlohmann/json to vcpkg, but gsheets vendors json.hpp. Which approach?**
A: **Vendor `json.hpp`** (mirror gsheets) + `include_directories(third_party)`; no vcpkg change.

## Related Research
- None yet (this is the first research artifact for this feature).

## Open Questions
- Exact predicate forms recognized for pushdown (operators on `start`/`end`, `BETWEEN`, half-open ranges) — design must specify the binder's filter→`timeMin`/`timeMax` extraction, accounting for the API's exclusive/asymmetric overlap semantics (timeMin filters event end, timeMax filters event start) and the residual exact-filter strategy (favor `pushdown_complex_filter`).
- Whether nested-field JSON columns (`attendees`/`recurrence`/`reminders`/`conference_data`) are writable round-trip in v1 or read-only first — note the API's array-overwrite behavior on update (arrays fully replace).
- Exact retry/backoff parameters (max attempts, base delay, jitter) and which 403 reasons (`rateLimitExceeded`/`userRateLimitExceeded`) are treated as retryable vs terminal (`quotaExceeded`/`forbiddenForNonOrganizer`).
- How DuckDB's row-id mechanism interacts with a `VARCHAR` event-id rowid vs the conventional `row_t` (int64) — whether a scan-time `row_t → event id` mapping side-table is needed (as SQLite/Postgres scanners do) or the VARCHAR virtual column flows through directly.
- Fate of the `reference/duckdb_gsheets` submodule before release (dev-only vs vendored/removed) so it doesn't ship in the distributed extension.
