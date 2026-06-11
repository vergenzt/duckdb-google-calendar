---
date: 2026-06-10T14:13:37-0400
author: Tim Vergenz
commit: 418af6f
branch: main
repository: duckdb-google-calendar
topic: "Google Calendar read/write DuckDB catalog extension (ATTACH StorageExtension)"
tags: [plan, duckdb-extension, storage-extension, catalog, google-calendar, gsheets-port, dml, filter-pushdown]
status: ready
parent: ".rpiv/artifacts/designs/2026-06-09_13-32-55_google-calendar-catalog-extension.md"
phase_count: 10
phases:
  - { n: 1, title: Scaffold rename + build foundation }
  - { n: 2, title: Transport + retry + mock seam + util (port) }
  - { n: 3, title: Auth + secret type + client (port + rebind) }
  - { n: 4, title: Calendar types + resources }
  - { n: 5, title: StorageExtension + Catalog + Transaction(Manager) }
  - { n: 6, title: Schema + table entries + enumeration }
  - { n: 7, title: Scan TableFunction (pushdown + pagination) }
  - { n: 8, title: DML operators (Insert/Update/Delete) }
  - { n: 9, title: MERGE (PlanMergeInto) }
  - { n: 10, title: Tests + docs }
last_updated: 2026-06-10T14:13:37-0400
last_updated_by: Tim Vergenz
---

# Google Calendar read/write DuckDB catalog extension Implementation Plan

## Overview

We register a custom DuckDB `StorageExtension` for `ATTACH '<account>' AS cal (TYPE google_calendar)`. The attach callback enumerates calendars via the Calendar API `calendarList` and builds a custom `Catalog` with one schema containing one `events` `TableCatalogEntry` per calendar. Reads are a `TableFunction` with `pushdown_complex_filter` (extracting `timeMin`/`timeMax` from `start` predicates, retaining the exact residual, paginating `nextPageToken`); writes are custom `EXTENSION` `PhysicalOperator` sinks (`INSERT`/`UPDATE`/`DELETE`/`MERGE`) issuing one HTTP call per row. The gsheets transport/auth/resource/JSON client subtree is ported into `src/calendar/` (namespace `duckdb::gcal`) with literals rebound; the retry decorator and SQL-level mock-HTTP seam are net-new.

This plan inherits 10 vertical slices (1:1) from the design artifact and is intended for phase-by-phase execution. The decomposition is **foundation-first**: `EXTENSION_SOURCES` (Phase 1) references every future source, so intermediate phases do **not** build standalone. Per-phase Automated Verification is therefore structural (grep / compile-unit presence); the full `make` / `make test` gate lives on the terminal phase (Phase 10).

> **Code placement for multi-touch files.** The design's `## Architecture` provides one end-state code block per file. Where a file appears in several phases' `**Files**:` lists (`src/google_calendar_extension.cpp`, `src/storage/calendar_catalog.{hpp,cpp}`, `src/storage/calendar_table_entry.cpp`, `src/calendar/auth_factory.{hpp,cpp}`), the full code block lives in the phase the design annotates as that snapshot's state; the other touching phases describe their delta and point to that block. The design provides no intermediate "stub" code, so those deltas are change summaries, not new code (do not invent stub bodies). See the per-phase notes.

## Desired End State

```sql
-- one-time credential setup (service account)
CREATE SECRET cal_sa (TYPE google_calendar, PROVIDER key_file, filepath '/path/sa.json');

-- attach the account: one table per calendar
ATTACH 'me' AS cal (TYPE google_calendar, SECRET cal_sa);
SHOW TABLES FROM cal;            -- primary, work_calendar, ...

-- read (time bound REQUIRED)
SELECT id, summary, start, "end"
FROM cal.primary
WHERE start >= TIMESTAMPTZ '2026-06-01 00:00-04' AND start < TIMESTAMPTZ '2026-07-01 00:00-04';

-- write
INSERT INTO cal.primary (summary, start, "end")
VALUES ('Standup', TIMESTAMPTZ '2026-06-10 09:00-04', TIMESTAMPTZ '2026-06-10 09:15-04');

UPDATE cal.primary SET location = 'Room 4' WHERE id = 'abc123';
DELETE FROM cal.primary WHERE id = 'abc123';

MERGE INTO cal.primary AS t USING staging AS s ON t.id = s.id
  WHEN MATCHED THEN UPDATE SET summary = s.summary
  WHEN NOT MATCHED THEN INSERT (summary, start, "end") VALUES (s.summary, s.start, s."end");
```

"Done" = the extension builds, `make test` is green without credentials (smoke passes, live test skips), and with `GOOGLE_CALENDAR_ACCESS_TOKEN` set the live test attaches and reads a bounded window from `cal.primary`.

## What We're NOT Doing

- C++ Catch2 unit harness (sqllogictest is the verification surface for v1).
- Mock-backed deterministic sqllogictest suite (`attach`/`scan_pushdown`/`dml`/`merge`/`retry` + `test/fixtures/*.json`) — **deferred to v2**. The env-var mock seam (Phase 2) ships but is unexercised by v1 tests; per-test fixtures require switching it to a per-connection DuckDB setting + a request-matching mock.
- DuckDB `JSON()` logical type for nested columns (VARCHAR raw-JSON passthrough instead — avoids a hard json-extension dependency).
- int64 rowid + scan-time `int64→id` mapping side-table (VARCHAR rowid is used directly).
- `PATCH` transport (`HttpMethod` stays `{GET,POST,PUT,DEL}`; UPDATE is read-modify-write PUT).
- Parallel API calls (DML serialized for predictable rate-limit behavior).
- Write-time validation of nested-JSON columns (caller-supplied text sent as-is; API array-replace semantics documented).
- gsheets-style replacement scans / global `read_*` table function (reads go only through the catalog table's `GetScanFunction`).
- ~~Removal of the `reference/duckdb_gsheets` submodule~~ — **now in scope**: the submodule is deleted in Phase 1 immediately after `json.hpp` is vendored (its only build-time use), so it cannot leak into the distributed extension (resolves the design's release-checklist note).
- `CREATE TABLE` / `DROP` / `ALTER` / index DDL on the calendar catalog (schema is fixed and enumerated; DDL pure-virtuals throw `NotImplementedException`).

---

## Phase 1: Scaffold rename + build foundation

### Overview
Rename the `waddle` template to `google_calendar` across src/build/CI/test via the bootstrap script, vendor `json.hpp`, and wire the build (include dirs, full `EXTENSION_SOURCES`, OpenSSL). `EXTENSION_SOURCES` deliberately lists every future source — intermediate phases will not build until Phase 10.

**Mechanism**: run `python3 scripts/bootstrap-template.py google_calendar` first (renames every `waddle` site + removes template-only files), then apply the build-wiring + entry-stub edits below and `cp` the vendored json header.

### Changes Required:

#### 1. Build wiring
**File**: `CMakeLists.txt`
**Changes**: Target rename to `google_calendar`, `include_directories(third_party)` + httplib, full 31-source `EXTENSION_SOURCES`, OpenSSL link on both static + loadable targets.

```cmake
cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME google_calendar)

find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)
include_directories(third_party)
include_directories(duckdb/third_party/httplib)

set(EXTENSION_SOURCES
    src/google_calendar_extension.cpp
    src/calendar_auth.cpp
    src/calendar/transport/http_client.cpp
    src/calendar/transport/httplib_client.cpp
    src/calendar/transport/mock_http_client.cpp
    src/calendar/transport/retrying_http_client.cpp
    src/calendar/transport/client_factory.cpp
    src/calendar/util/encoding.cpp
    src/calendar/util/options.cpp
    src/calendar/util/secret.cpp
    src/calendar/util/proxy.cpp
    src/calendar/util/version.cpp
    src/calendar/util/query.cpp
    src/calendar/auth/service_account_auth.cpp
    src/calendar/auth/bearer_token_auth.cpp
    src/calendar/auth_factory.cpp
    src/calendar/resources/base.cpp
    src/calendar/resources/calendar_list.cpp
    src/calendar/resources/events.cpp
    src/storage/calendar_storage_extension.cpp
    src/storage/calendar_catalog.cpp
    src/storage/calendar_transaction_manager.cpp
    src/storage/calendar_transaction.cpp
    src/storage/calendar_schema_entry.cpp
    src/storage/calendar_table_entry.cpp
    src/storage/event_schema.cpp
    src/storage/calendar_scan.cpp
    src/storage/calendar_insert.cpp
    src/storage/calendar_update.cpp
    src/storage/calendar_delete.cpp
    src/storage/calendar_merge.cpp)

# Warn on unused/dead code (GCC/Clang only)
add_compile_options($<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wunused-function>)
add_compile_options($<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wunreachable-code>)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenSSL in both the static library and the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
```

#### 2. Extension load config
**File**: `extension_config.cmake`
**Changes**: Target rename + `LOAD_TESTS`.

```cmake
# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(google_calendar
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
```

#### 3. Vendored JSON header + remove the reference submodule
**File**: `third_party/json.hpp`
**Changes**: Copy verbatim from the gsheets reference (do not hand-edit), then delete the `reference/duckdb_gsheets` submodule. The `cp` MUST run before the deletion — `json.hpp` is the only build-time dependency on the reference subtree; all other ports are carried as full code in this plan's fences. Deleting it here (the first implementation phase) guarantees it cannot leak into the distributed extension. The design's `## Pattern References` paths remain readable in the design artifact for cross-checking during Phases 2–9, but the live files will be gone — transcribe from this plan's code fences.

```text
# Vendored verbatim from the gsheets reference (nlohmann/json single-header, ~920 KB),
# then remove the dev-only reference submodule so it cannot ship in the distributed extension.
# Implement copies the header (do not hand-edit / inline), THEN deletes the submodule:
cp reference/duckdb_gsheets/third_party/json.hpp third_party/json.hpp
git submodule deinit -f reference/duckdb_gsheets
git rm -f reference/duckdb_gsheets
rm -rf .git/modules/reference/duckdb_gsheets
# (also drop the [submodule "reference/duckdb_gsheets"] stanza from .gitmodules if present)
```

#### 4. Extension class declaration
**File**: `src/include/google_calendar_extension.hpp`
**Changes**: Renamed extension class.

```cpp
#pragma once

#include "duckdb.hpp"

namespace duckdb {

class GoogleCalendarExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
```

#### 5. Entry point (stub)
**File**: `src/google_calendar_extension.cpp`
**Changes**: After `bootstrap-template.py` renames the file + entry macro, this phase leaves a minimal `LoadInternal` (OpenSSL init only; secret registration arrives in Phase 3, storage-extension registration in Phase 5). **Full final code block lives in Phase 5** (design-annotated "Slice 5 state") — do not paste the Phase 5 secret/storage registration here, since the referenced symbols (`CreateGoogleCalendarSecretFunctions`, `CalendarStorageExtension`) do not exist until Phases 3/5.

### Success Criteria:

#### Automated Verification:
- [x] No `waddle` literals remain: `grep -ril waddle . --exclude-dir=duckdb --exclude-dir=reference --exclude-dir=extension-ci-tools --exclude-dir=.git --exclude-dir=.rpiv --exclude-dir=.pi --exclude-dir=docs | wc -l` returns 0
- [x] Reference submodule removed: `test ! -e reference/duckdb_gsheets && echo ok` (deleted after vendoring `json.hpp`)
- [x] CMake target renamed: `grep -c 'set(TARGET_NAME google_calendar)' CMakeLists.txt` returns 1
- [x] Vendored json present: `test -f third_party/json.hpp && echo ok`
- [x] Include dirs wired: `grep -c 'include_directories(third_party)' CMakeLists.txt` returns 1 and `grep -c 'duckdb/third_party/httplib' CMakeLists.txt` returns 1
- [x] LOAD_TESTS present: `grep -c LOAD_TESTS extension_config.cmake` returns 1
- [x] Entry macro renamed: `grep -c 'DUCKDB_CPP_EXTENSION_ENTRY(google_calendar' src/google_calendar_extension.cpp` returns 1
- [x] `EXTENSION_SOURCES` lists 31 `.cpp` sources: `grep -c '\.cpp' CMakeLists.txt` returns 31

#### Manual Verification:
- [ ] `scripts/bootstrap-template.py` ran cleanly (renamed src/test files, removed `ExtensionTemplate.yml` + the script itself)
- [ ] `.github/workflows/MainDistributionPipeline.yml` `extension_name` reads `google_calendar`
- [ ] `EXTENSION_SOURCES` matches the File Map `.cpp` set exactly (no omissions/orphans)

---

## Phase 2: Transport + retry + mock seam + util (port)

### Overview
Port the gsheets transport + util subtree into `src/calendar/` under namespace `duckdb::gcal`, rebinding namespace/include literals. Add the net-new `RetryingHttpClient` decorator and the `CreateHttpClient` env-var mock seam (`GOOGLE_CALENDAR_TEST_FIXTURE`).

### Changes Required:

#### 1. HTTP POD types
**File**: `src/include/calendar/transport/http_type.hpp`
**Changes**: HTTP method/request/response/proxy POD types.

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <map>

namespace duckdb {
namespace gcal {

enum class HttpMethod { GET, POST, PUT, DEL };

using HttpHeaders = std::map<std::string, std::string>;

struct HttpRequest {
	HttpMethod method;
	std::string url;
	HttpHeaders headers;
	std::string body;
};

struct HttpResponse {
	int statusCode;
	HttpHeaders headers;
	std::string body;
};

struct HttpProxyConfig {
	std::string host;
	uint16_t port = 0;
	std::string username;
	std::string password;
};

} // namespace gcal
} // namespace duckdb
```

#### 2. IHttpClient interface + verb helpers
**File**: `src/include/calendar/transport/http_client.hpp`

```cpp
#pragma once

#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

class IHttpClient {
public:
	virtual ~IHttpClient() = default;
	virtual HttpResponse Execute(const HttpRequest &request) = 0;

	HttpRequest BuildRequest(const HttpMethod method, const std::string &url, const HttpHeaders &headers);
	HttpRequest BuildRequest(const HttpMethod method, const std::string &url, const HttpHeaders &headers,
	                         const std::string &body);

	HttpResponse Get(const std::string &url, const HttpHeaders &headers);
	HttpResponse Post(const std::string &url, const HttpHeaders &headers, const std::string &body);
	HttpResponse Put(const std::string &url, const HttpHeaders &headers, const std::string &body);
	HttpResponse Delete(const std::string &url, const HttpHeaders &headers);
};
} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/transport/http_client.cpp`

```cpp
#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

HttpRequest IHttpClient::BuildRequest(const HttpMethod method, const std::string &url, const HttpHeaders &headers) {
	return HttpRequest {method, url, headers, ""};
}

HttpRequest IHttpClient::BuildRequest(const HttpMethod method, const std::string &url, const HttpHeaders &headers,
                                      const std::string &body) {
	return HttpRequest {method, url, headers, body};
}

HttpResponse IHttpClient::Get(const std::string &url, const HttpHeaders &headers) {
	return Execute(BuildRequest(HttpMethod::GET, url, headers));
}

HttpResponse IHttpClient::Post(const std::string &url, const HttpHeaders &headers, const std::string &body) {
	return Execute(BuildRequest(HttpMethod::POST, url, headers, body));
}

HttpResponse IHttpClient::Put(const std::string &url, const HttpHeaders &headers, const std::string &body) {
	return Execute(BuildRequest(HttpMethod::PUT, url, headers, body));
}

HttpResponse IHttpClient::Delete(const std::string &url, const HttpHeaders &headers) {
	return Execute(BuildRequest(HttpMethod::DEL, url, headers));
}
} // namespace gcal
} // namespace duckdb
```

#### 3. Real cpp-httplib client
**File**: `src/include/calendar/transport/httplib_client.hpp`

```cpp
#pragma once

#include <utility>

#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

class HttpLibClient : public IHttpClient {
public:
	explicit HttpLibClient(HttpProxyConfig proxy_config) : proxy_config(std::move(proxy_config)) {
	}

	HttpResponse Execute(const HttpRequest &request) override;

private:
	void ParseUrl(const std::string &url, std::string &baseUrl, std::string &path);

	HttpProxyConfig proxy_config;
};
} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/transport/httplib_client.cpp`

```cpp
#include "duckdb/common/exception.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include "calendar/transport/httplib_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

void HttpLibClient::ParseUrl(const std::string &url, std::string &baseUrl, std::string &path) {
	const std::string schemaSep = "://";
	size_t schemeEnd = url.find(schemaSep);
	if (schemeEnd == std::string::npos) {
		throw duckdb::IOException("Invalid URL: " + url);
	}

	size_t pathStart = url.find('/', schemeEnd + schemaSep.size());
	if (pathStart == std::string::npos) {
		baseUrl = url;
		path = "/";
	} else {
		baseUrl = url.substr(0, pathStart);
		path = url.substr(pathStart);
	}
}

HttpResponse HttpLibClient::Execute(const HttpRequest &request) {
	std::string baseUrl;
	std::string path;
	ParseUrl(request.url, baseUrl, path);

	duckdb_httplib_openssl::Client client(baseUrl);
	if (!proxy_config.host.empty()) {
		client.set_proxy(proxy_config.host, proxy_config.port);
		if (!proxy_config.username.empty()) {
			client.set_proxy_basic_auth(proxy_config.username, proxy_config.password);
		}
	}

	std::string contentType = "application/json";
	duckdb_httplib_openssl::Headers headers;
	for (const auto &h : request.headers) {
		if (h.first == "Content-Type") {
			contentType = h.second;
		} else {
			headers.insert(h);
		}
	}

	duckdb_httplib_openssl::Result result;

	switch (request.method) {
	case HttpMethod::GET:
		result = client.Get(path, headers);
		break;
	case HttpMethod::POST:
		result = client.Post(path, headers, request.body, contentType);
		break;
	case HttpMethod::PUT:
		result = client.Put(path, headers, request.body, contentType);
		break;
	case HttpMethod::DEL:
		result = client.Delete(path, headers);
		break;
	}

	HttpResponse response;

	if (!result) {
		throw duckdb::IOException("HTTP request failed: " + duckdb_httplib_openssl::to_string(result.error()));
	}

	response.statusCode = result->status;
	response.body = result->body;
	for (const auto &h : result->headers) {
		response.headers[h.first] = h.second;
	}
	return response;
}

} // namespace gcal
} // namespace duckdb
```

#### 4. Mock client (queue + recorder)
**File**: `src/include/calendar/transport/mock_http_client.hpp`

```cpp
#pragma once

#include <vector>

#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

class MockHttpClient : public IHttpClient {
public:
	HttpResponse Execute(const HttpRequest &request) override;
	void AddResponse(HttpResponse response);
	const std::vector<HttpRequest> &GetRecordedRequests() const;

private:
	size_t responseIndex = 0;
	std::vector<HttpResponse> responses;
	std::vector<HttpRequest> recordedRequests;
};
} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/transport/mock_http_client.cpp`

```cpp
#include "calendar/transport/mock_http_client.hpp"
#include <stdexcept>

namespace duckdb {
namespace gcal {

HttpResponse MockHttpClient::Execute(const HttpRequest &request) {
	recordedRequests.push_back(request);
	if (responseIndex < responses.size()) {
		return responses[responseIndex++];
	}
	throw std::runtime_error("MockHttpClient: No more responses queued");
}

void MockHttpClient::AddResponse(HttpResponse response) {
	responses.push_back(std::move(response));
}

const std::vector<HttpRequest> &MockHttpClient::GetRecordedRequests() const {
	return recordedRequests;
}
} // namespace gcal
} // namespace duckdb
```

#### 5. Retry decorator (net-new)
**File**: `src/include/calendar/transport/retrying_http_client.hpp`

```cpp
#pragma once

#include <memory>

#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

struct RetryConfig {
	int max_attempts = 5;
	int base_delay_ms = 1000;
	bool zero_sleep = false; // tests set this so the suite stays fast
};

class RetryingHttpClient : public IHttpClient {
public:
	RetryingHttpClient(std::unique_ptr<IHttpClient> inner, RetryConfig config = RetryConfig());
	HttpResponse Execute(const HttpRequest &request) override;
	static bool IsRetryable(const HttpResponse &response);

private:
	std::unique_ptr<IHttpClient> inner;
	RetryConfig config;
	void SleepBackoff(int attempt);
};
} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/transport/retrying_http_client.cpp`

```cpp
#include "calendar/transport/retrying_http_client.hpp"

#include <chrono>
#include <random>
#include <thread>

namespace duckdb {
namespace gcal {

RetryingHttpClient::RetryingHttpClient(std::unique_ptr<IHttpClient> inner, RetryConfig config)
    : inner(std::move(inner)), config(config) {
}

bool RetryingHttpClient::IsRetryable(const HttpResponse &response) {
	int s = response.statusCode;
	if (s == 429) {
		return true;
	}
	if (s >= 500 && s <= 599) {
		return true;
	}
	if (s == 403) {
		const auto &b = response.body;
		if (b.find("rateLimitExceeded") != std::string::npos ||
		    b.find("userRateLimitExceeded") != std::string::npos) {
			return true;
		}
	}
	return false;
}

void RetryingHttpClient::SleepBackoff(int attempt) {
	if (config.zero_sleep) {
		return;
	}
	// Exponential backoff with full jitter: delay in [0, base * 2^attempt].
	long long base = (long long)config.base_delay_ms * (1LL << attempt);
	static thread_local std::mt19937 rng(std::random_device {}());
	std::uniform_int_distribution<long long> dist(0, base);
	long long delay = dist(rng);
	std::this_thread::sleep_for(std::chrono::milliseconds(delay));
}

HttpResponse RetryingHttpClient::Execute(const HttpRequest &request) {
	HttpResponse response;
	for (int attempt = 0; attempt < config.max_attempts; attempt++) {
		response = inner->Execute(request);
		if (!IsRetryable(response) || attempt + 1 == config.max_attempts) {
			return response;
		}
		SleepBackoff(attempt);
	}
	return response;
}
} // namespace gcal
} // namespace duckdb
```

#### 6. Client factory (real/mock branch + retry wrap)
**File**: `src/include/calendar/transport/client_factory.hpp`

```cpp
#pragma once

#include <memory>

#include "duckdb/main/client_context.hpp"

#include "calendar/transport/http_client.hpp"

namespace duckdb {
namespace gcal {

std::unique_ptr<IHttpClient> CreateHttpClient(ClientContext &ctx);

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/transport/client_factory.cpp`

```cpp
#include <cstdlib>
#include <fstream>

#include "json.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

#include "calendar/transport/client_factory.hpp"
#include "calendar/transport/httplib_client.hpp"
#include "calendar/transport/mock_http_client.hpp"
#include "calendar/transport/retrying_http_client.hpp"
#include "calendar/util/proxy.hpp"

using json = nlohmann::json;

namespace duckdb {
namespace gcal {

// Test-only: load a queue of canned responses from a fixture file. Format:
//   { "responses": [ { "status": 429, "body": "..." }, { "status": 200, "body": "{...}" } ] }
static std::unique_ptr<MockHttpClient> LoadMockFromFixture(const std::string &path) {
	std::ifstream ifs(path);
	if (!ifs.is_open()) {
		throw IOException("Could not open GOOGLE_CALENDAR_TEST_FIXTURE at: " + path);
	}
	json fixture = json::parse(ifs);
	auto mock = make_uniq<MockHttpClient>();
	for (const auto &entry : fixture.at("responses")) {
		HttpResponse response;
		response.statusCode = entry.value("status", 200);
		response.body = entry.value("body", std::string());
		if (entry.contains("headers")) {
			for (auto it = entry["headers"].begin(); it != entry["headers"].end(); ++it) {
				response.headers[it.key()] = it.value().get<std::string>();
			}
		}
		mock->AddResponse(std::move(response));
	}
	return mock;
}

std::unique_ptr<IHttpClient> CreateHttpClient(ClientContext &ctx) {
	RetryConfig retry_config;

	const char *fixture = std::getenv("GOOGLE_CALENDAR_TEST_FIXTURE");
	if (fixture && *fixture) {
		// Test seam: deterministic, credential-free; no real backoff sleeps under tests.
		retry_config.zero_sleep = true;
		return make_uniq<RetryingHttpClient>(LoadMockFromFixture(fixture), retry_config);
	}

	auto proxy_config = GetHttpProxyConfig(ctx);
	auto real = make_uniq<HttpLibClient>(proxy_config);
	return make_uniq<RetryingHttpClient>(std::move(real), retry_config);
}

} // namespace gcal
} // namespace duckdb
```

#### 7. Util — encoding (base64url + PEM)
**File**: `src/include/calendar/util/encoding.hpp`

```cpp
#pragma once

#include <string>

namespace duckdb {
namespace gcal {

constexpr unsigned char MASK_TOP6 = 0xFC;
constexpr unsigned char MASK_BOT2 = 0x03;
constexpr unsigned char MASK_TOP4 = 0xF0;
constexpr unsigned char MASK_BOT4 = 0x0F;
constexpr unsigned char MASK_TOP2 = 0xC0;
constexpr unsigned char MASK_BOT6 = 0x3F;

std::string Base64UrlEncode(const unsigned char *data, size_t len);

std::string Base64UrlEncode(const std::string &input);

std::string NormalizePemKey(const std::string &key);

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/util/encoding.cpp`

```cpp
#include "calendar/util/encoding.hpp"

namespace duckdb {
namespace gcal {

static const char BASE64URL_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char GetBase64UrlChar(unsigned char sixBits) {
	return BASE64URL_ALPHABET[sixBits];
}

std::string Base64UrlEncode(const unsigned char *data, size_t len) {
	std::string result;
	size_t i = 0;

	while (i < len) {
		unsigned char b0 = data[i];
		unsigned char b1 = (i + 1 < len) ? data[i + 1] : 0;
		unsigned char b2 = (i + 2 < len) ? data[i + 2] : 0;

		result += GetBase64UrlChar((b0 & MASK_TOP6) >> 2);
		result += GetBase64UrlChar(((b0 & MASK_BOT2) << 4) | ((b1 & MASK_TOP4) >> 4));

		if (i + 1 < len) {
			result += GetBase64UrlChar(((b1 & MASK_BOT4) << 2) | ((b2 & MASK_TOP2) >> 6));
		}
		if (i + 2 < len) {
			result += GetBase64UrlChar(b2 & MASK_BOT6);
		}
		i += 3;
	}
	return result;
}

std::string Base64UrlEncode(const std::string &input) {
	return Base64UrlEncode(reinterpret_cast<const unsigned char *>(input.c_str()), input.length());
}

std::string NormalizePemKey(const std::string &key) {
	std::string pem = key;
	size_t pos = 0;
	while ((pos = pem.find("\\n", pos)) != std::string::npos) {
		pem.replace(pos, 2, "\n");
		pos += 1;
	}
	return pem;
}

} // namespace gcal
} // namespace duckdb
```

#### 8. Util — options
**File**: `src/include/calendar/util/options.hpp`

```cpp
#pragma once

#include <string>
#include <utility>

#include "duckdb/common/types/value.hpp"

namespace duckdb {
namespace gcal {

std::string GetStringOption(const case_insensitive_map_t<vector<Value>> &options, const std::string &name,
                            const std::string &default_value = "");

std::string GetStringOption(const case_insensitive_map_t<Value> &options, const std::string &name,
                            const std::string &default_value = "");

std::pair<bool, bool> GetBoolOption(const case_insensitive_map_t<vector<Value>> &options, const std::string &name,
                                    bool default_value = false);

std::pair<bool, bool> GetBoolOption(const case_insensitive_map_t<Value> &options, const std::string &name,
                                    bool default_value = false);

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/util/options.cpp`

```cpp
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/types/value.hpp"

#include "calendar/util/options.hpp"

std::string duckdb::gcal::GetStringOption(const case_insensitive_map_t<vector<Value>> &options,
                                          const std::string &name, const std::string &default_value) {
	const auto it = options.find(name);
	if (it == options.end()) {
		return default_value;
	}
	std::string err;
	Value val;
	if (!it->second.back().DefaultTryCastAs(LogicalType::VARCHAR, val, &err)) {
		throw BinderException(name + " option must be VARCHAR");
	}
	if (val.IsNull()) {
		throw BinderException(name + " option must not be NULL");
	}
	return StringValue::Get(val);
}

std::string duckdb::gcal::GetStringOption(const case_insensitive_map_t<Value> &options, const std::string &name,
                                          const std::string &default_value) {
	const auto it = options.find(name);
	if (it == options.end()) {
		return default_value;
	}
	std::string err;
	Value val;
	if (!it->second.DefaultTryCastAs(LogicalType::VARCHAR, val, &err)) {
		throw BinderException(name + " option must be VARCHAR");
	}
	if (val.IsNull()) {
		throw BinderException(name + " option must not be NULL");
	}
	return StringValue::Get(val);
}

std::pair<bool, bool> duckdb::gcal::GetBoolOption(const case_insensitive_map_t<vector<Value>> &options,
                                                  const std::string &name, bool default_value) {
	const auto it = options.find(name);
	if (it == options.end()) {
		return std::make_pair(default_value, false);
	}
	if (it->second.size() != 1) {
		throw BinderException(name + " option must be a single boolean value");
	}
	std::string err;
	Value val;
	if (!it->second.back().DefaultTryCastAs(LogicalType::BOOLEAN, val, &err)) {
		throw BinderException(name + " option must be a single boolean value");
	}
	if (val.IsNull()) {
		throw BinderException(name + " option must be a single boolean value");
	}
	return std::make_pair(BooleanValue::Get(val), true);
}

std::pair<bool, bool> duckdb::gcal::GetBoolOption(const case_insensitive_map_t<Value> &options,
                                                  const std::string &name, bool default_value) {
	const auto it = options.find(name);
	if (it == options.end()) {
		return std::make_pair(default_value, false);
	}
	std::string err;
	Value val;
	if (!it->second.DefaultTryCastAs(LogicalType::BOOLEAN, val, &err)) {
		throw BinderException(name + " option must be a single boolean value");
	}
	if (val.IsNull()) {
		throw BinderException(name + " option must be a single boolean value");
	}
	return std::make_pair(BooleanValue::Get(val), true);
}
```

#### 9. Util — secret lookup
**File**: `src/include/calendar/util/secret.hpp`

```cpp
#pragma once

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
namespace gcal {

const SecretMatch GetSecretMatch(ClientContext &ctx, const std::string &path, const std::string &type);

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/util/secret.cpp`

```cpp
#include "calendar/util/secret.hpp"

#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {
namespace gcal {

const SecretMatch GetSecretMatch(ClientContext &ctx, const std::string &path, const std::string &type) {
	auto &manager = SecretManager::Get(ctx);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(ctx);
	return manager.LookupSecret(transaction, path, type);
}

} // namespace gcal
} // namespace duckdb
```

#### 10. Util — response template
**File**: `src/include/calendar/util/response.hpp`

```cpp
#pragma once

#include "json.hpp"

#include "calendar/exception.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

template <typename T>
T ParseResponse(const HttpResponse &response) {
	if (response.statusCode != 200) {
		throw CalendarApiException(response.statusCode, response.body);
	}
	try {
		return nlohmann::json::parse(response.body).get<T>();
	} catch (const nlohmann::json::exception &e) {
		throw CalendarParseException("Failed to parse response: " + std::string(e.what()));
	}
}

} // namespace gcal
} // namespace duckdb
```

#### 11. Util — proxy config
**File**: `src/include/calendar/util/proxy.hpp`

```cpp
#pragma once

#include "duckdb/main/client_context.hpp"

#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

HttpProxyConfig GetHttpProxyConfig(ClientContext &ctx);

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/util/proxy.cpp`

```cpp
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/secret/secret.hpp"

#include "calendar/util/proxy.hpp"
#include "calendar/util/secret.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

static void ParseHTTPProxyHost(string &proxy_value, string &hostname_out, uint16_t &port_out) {
	uint16_t default_port = 80;
	auto sanitized_proxy_value = proxy_value;
	if (StringUtil::StartsWith(proxy_value, "http://")) {
		sanitized_proxy_value = proxy_value.substr(7);
	} else if (StringUtil::StartsWith(proxy_value, "https://")) {
		default_port = 443;
		sanitized_proxy_value = proxy_value.substr(8);
	}

	while (!sanitized_proxy_value.empty() && StringUtil::EndsWith(sanitized_proxy_value, "/")) {
		sanitized_proxy_value.pop_back();
	}

	auto proxy_split = StringUtil::Split(sanitized_proxy_value, ":");
	if (proxy_split.size() == 1) {
		hostname_out = proxy_split[0];
		port_out = default_port;
	} else if (proxy_split.size() == 2) {
		uint16_t port;
		try {
			auto val = std::stoul(proxy_split[1]);
			if (val > std::numeric_limits<uint16_t>::max()) {
				throw InvalidInputException("Failed to parse port from http_proxy '%s'", proxy_value);
			}
			port = static_cast<uint16_t>(val);
		} catch (const std::invalid_argument &e) {
			throw InvalidInputException("Failed to parse port from http_proxy '%s'", proxy_value);
		} catch (const std::out_of_range &e) {
			throw InvalidInputException("Failed to parse port from http_proxy '%s'", proxy_value);
		}
		hostname_out = proxy_split[0];
		port_out = port;
	} else {
		throw InvalidInputException("Failed to parse http_proxy '%s' into a host and port", proxy_value);
	}
}

HttpProxyConfig GetHttpProxyConfig(ClientContext &ctx) {
	HttpProxyConfig proxy_config;
	auto match = GetSecretMatch(ctx, "", "http");
	if (match.HasMatch()) {
		auto &secret = match.GetSecret();
		auto http_secret = dynamic_cast<const KeyValueSecret *>(&secret);

		Value http_proxy, http_proxy_username, http_proxy_password;

		if (http_secret->TryGetValue("http_proxy", http_proxy)) {
			auto proxy_value = http_proxy.ToString();
			if (!proxy_value.empty()) {
				ParseHTTPProxyHost(proxy_value, proxy_config.host, proxy_config.port);

				if (http_secret->TryGetValue("http_proxy_username", http_proxy_username)) {
					proxy_config.username = http_proxy_username.ToString();
				}

				if (http_secret->TryGetValue("http_proxy_password", http_proxy_password)) {
					proxy_config.password = http_proxy_password.ToString();
				}
			}
		}
	} else {
		Value http_proxy, http_proxy_username, http_proxy_password;
		ctx.TryGetCurrentSetting("http_proxy", http_proxy);

		auto proxy_value = http_proxy.ToString();

		if (!proxy_value.empty()) {
			ParseHTTPProxyHost(proxy_value, proxy_config.host, proxy_config.port);
			ctx.TryGetCurrentSetting("http_proxy_username", http_proxy_username);
			ctx.TryGetCurrentSetting("http_proxy_password", http_proxy_password);
			proxy_config.username = http_proxy_username.ToString();
			proxy_config.password = http_proxy_password.ToString();
		}
	}
	return proxy_config;
}

} // namespace gcal
} // namespace duckdb
```

#### 12. Util — version
**File**: `src/include/calendar/util/version.hpp`

```cpp
#pragma once

#include <string>

namespace duckdb {

/**
 * Retrieves version from macro if present or empty string if not
 */
std::string getVersion();

} // namespace duckdb
```

**File**: `src/calendar/util/version.cpp`

```cpp
#include "calendar/util/version.hpp"

std::string duckdb::getVersion() {
#ifdef EXT_VERSION_GOOGLE_CALENDAR
	return EXT_VERSION_GOOGLE_CALENDAR;
#else
	return "";
#endif
}
```

#### 13. Calendar exceptions
**File**: `src/include/calendar/exception.hpp`

```cpp
#pragma once

#include <stdexcept>
#include <string>

namespace duckdb {
namespace gcal {

class CalendarException : public std::runtime_error {
public:
	explicit CalendarException(const std::string &message) : std::runtime_error(message) {
	}
};

class CalendarApiException : public CalendarException {
public:
	CalendarApiException(int statusCode, const std::string &apiMessage)
	    : CalendarException("Google Calendar API error (" + std::to_string(statusCode) + "): " + apiMessage),
	      statusCode(statusCode), apiMessage(apiMessage) {
	}
	int GetStatusCode() const {
		return statusCode;
	}
	const std::string &GetApiMessage() const {
		return apiMessage;
	}

private:
	int statusCode;
	std::string apiMessage;
};

class CalendarParseException : public CalendarException {
public:
	explicit CalendarParseException(const std::string &message) : CalendarException(message) {
	}
};

class CalendarNotFoundException : public CalendarException {
public:
	explicit CalendarNotFoundException(const std::string &identifier)
	    : CalendarException("Calendar resource not found: " + identifier), identifier(identifier) {
	}
	const std::string &GetIdentifier() const {
		return identifier;
	}

private:
	std::string identifier;
};
} // namespace gcal
} // namespace duckdb
```

### Success Criteria:

#### Automated Verification:
- [x] No stale gsheets namespace in sources OR headers: `grep -rl 'namespace sheets' src/calendar src/include/calendar | wc -l` returns 0
- [x] No stale gsheets include paths: `grep -rn '"sheets/\|"utils/' src/calendar src/include/calendar | wc -l` returns 0
- [x] Retry decorator present: `grep -c 'class RetryingHttpClient' src/include/calendar/transport/retrying_http_client.hpp` returns 1
- [x] Mock seam env var wired: `grep -c 'GOOGLE_CALENDAR_TEST_FIXTURE' src/calendar/transport/client_factory.cpp` returns 1 (reads 2: getenv + error message — both in the plan's own code block)
- [x] Version macro rebound: `grep -c 'EXT_VERSION_GOOGLE_CALENDAR' src/calendar/util/version.cpp` returns 1 (reads 2: #ifdef + return — both in the plan's own code block) and `grep -c GSHEETS src/calendar/util/version.cpp` returns 0

#### Manual Verification:
- [ ] `RetryingHttpClient::IsRetryable` returns true for 429 / 5xx / 403-with-rateLimitExceeded|userRateLimitExceeded and false otherwise; `Execute` makes at most `max_attempts` calls and returns the last response on exhaustion
- [ ] Mock fixture JSON (`{"responses":[{"status":..,"body":..}]}`) seeds the queue in order; `make_uniq<RetryingHttpClient>(LoadMockFromFixture(...), cfg)` compiles (unique_ptr<MockHttpClient> → unique_ptr<IHttpClient>)
- [ ] (deferred to Phase 10) full build links the ported transport/util translation units

---

## Phase 3: Auth + secret type + client (port + rebind)

### Overview
Port the gsheets auth providers (service-account RS256 JWT, bearer token) and secret triad into `src/calendar/`, rebinding the OAuth scope to `.../auth/calendar`, the secret type to `google_calendar`, and reading the OAuth `client_id`/`redirect_uri` from the environment (D4a). Merge secret-type registration into the extension entry point.

### Changes Required:

#### 1. Auth provider interface
**File**: `src/include/calendar/auth/auth_provider.hpp`

```cpp
#pragma once

#include <string>

namespace duckdb {
namespace gcal {

class IAuthProvider {
public:
	virtual ~IAuthProvider() = default;
	virtual std::string GetAuthorizationHeader() = 0;
};

} // namespace gcal
} // namespace duckdb
```

#### 2. Service-account auth (RS256 JWT, calendar scope)
**File**: `src/include/calendar/auth/service_account_auth.hpp`

```cpp
#pragma once

#include <string>
#include <ctime>

#include "calendar/auth/auth_provider.hpp"
#include "calendar/transport/http_client.hpp"

namespace duckdb {
namespace gcal {

class ServiceAccountAuth : public IAuthProvider {
public:
	ServiceAccountAuth(IHttpClient &http, const std::string &email, const std::string &privateKey)
	    : http(http), email(email), privateKey(privateKey) {
	}

	std::string GetAuthorizationHeader() override;

private:
	IHttpClient &http;
	std::string email;
	std::string privateKey;
	std::string cachedToken;
	std::time_t expirationTime = 0;

	std::string CreateJwt();
	void ExchangeJwtForToken();
	bool IsExpired();
	void Refresh();
};
} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/auth/service_account_auth.cpp`

```cpp
#include "json.hpp"
#include "duckdb/common/exception.hpp"

#include "calendar/auth/service_account_auth.hpp"
#include "calendar/transport/http_type.hpp"
#include "calendar/util/encoding.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

using json = nlohmann::json;

namespace duckdb {
namespace gcal {

// RAII deleters for OpenSSL types
struct BIODeleter {
	void operator()(BIO *b) const {
		if (b) {
			BIO_free(b);
		}
	}
};
struct EVPPKEYDeleter {
	void operator()(EVP_PKEY *p) const {
		if (p) {
			EVP_PKEY_free(p);
		}
	}
};
struct EVPMDCTXDeleter {
	void operator()(EVP_MD_CTX *m) const {
		if (m) {
			EVP_MD_CTX_free(m);
		}
	}
};

using BIOPtr = std::unique_ptr<BIO, BIODeleter>;
using EVPPKEYPtr = std::unique_ptr<EVP_PKEY, EVPPKEYDeleter>;
using EVPMDCTXPtr = std::unique_ptr<EVP_MD_CTX, EVPMDCTXDeleter>;

constexpr int TOKEN_TTL = 1800;
constexpr const char *TOKEN_ENDPOINT = "https://oauth2.googleapis.com/token";

std::string ServiceAccountAuth::GetAuthorizationHeader() {
	if (IsExpired()) {
		Refresh();
	}
	return "Bearer " + cachedToken;
}

std::string ServiceAccountAuth::CreateJwt() {
	const char *header = R"({"alg":"RS256","typ":"JWT"})";

	json claimSet;
	std::time_t now = std::time(nullptr);
	claimSet["iss"] = email;
	claimSet["scope"] = "https://www.googleapis.com/auth/calendar";
	claimSet["aud"] = TOKEN_ENDPOINT;
	claimSet["iat"] = now;
	claimSet["exp"] = now + TOKEN_TTL;

	std::string headerB64 = Base64UrlEncode(header);
	std::string claimsB64 = Base64UrlEncode(claimSet.dump());
	std::string signInput = headerB64 + "." + claimsB64;

	auto pem = NormalizePemKey(privateKey);

	BIOPtr bio(BIO_new_mem_buf(pem.c_str(), -1));
	if (!bio) {
		throw duckdb::IOException("Failed to create BIO for private key");
	}

	EVPPKEYPtr pkey(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
	if (!pkey) {
		throw duckdb::IOException("Failed to parse private key");
	}

	EVPMDCTXPtr mdctx(EVP_MD_CTX_new());
	if (!mdctx) {
		throw duckdb::IOException("Failed to create EVP_MD_CTX");
	}

	if (EVP_DigestSignInit(mdctx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) != 1) {
		throw duckdb::IOException("Failed to initialize signing context");
	}
	if (EVP_DigestSignUpdate(mdctx.get(), signInput.c_str(), signInput.length()) != 1) {
		throw duckdb::IOException("Failed to update signing context");
	}

	size_t sigLen = 0;
	if (EVP_DigestSignFinal(mdctx.get(), nullptr, &sigLen) != 1) {
		throw duckdb::IOException("Failed to get signature length");
	}

	std::vector<unsigned char> signature(sigLen);
	if (EVP_DigestSignFinal(mdctx.get(), signature.data(), &sigLen) != 1) {
		throw duckdb::IOException("Failed to sign JWT");
	}

	return signInput + "." + Base64UrlEncode(signature.data(), sigLen);
}

void ServiceAccountAuth::ExchangeJwtForToken() {
	std::string jwt = CreateJwt();
	std::string body = "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=" + jwt;

	HttpHeaders headers;
	headers["Content-Type"] = "application/x-www-form-urlencoded";

	HttpResponse response = http.Post(TOKEN_ENDPOINT, headers, body);

	if (response.statusCode != 200) {
		throw duckdb::IOException("Token exchange failed: " + response.body);
	}

	json responseJson;
	try {
		responseJson = json::parse(response.body);
	} catch (const json::exception &) {
		throw duckdb::IOException("Failed to parse token response: " + response.body);
	}

	if (!responseJson.contains("access_token")) {
		throw duckdb::IOException("Token response missing 'access_token': " + response.body);
	}
	cachedToken = responseJson["access_token"].get<std::string>();

	int expiresIn = responseJson.value("expires_in", TOKEN_TTL);
	expirationTime = std::time(nullptr) + expiresIn - 60; // refresh 1 min early
}

bool ServiceAccountAuth::IsExpired() {
	if (cachedToken.empty()) {
		return true;
	}
	std::time_t now = std::time(nullptr);
	return now >= expirationTime;
}

void ServiceAccountAuth::Refresh() {
	ExchangeJwtForToken();
}

} // namespace gcal
} // namespace duckdb
```

#### 3. Bearer-token auth
**File**: `src/include/calendar/auth/bearer_token_auth.hpp`

```cpp
#pragma once

#include <string>

#include "calendar/auth/auth_provider.hpp"

namespace duckdb {
namespace gcal {

class BearerTokenAuth : public IAuthProvider {
public:
	explicit BearerTokenAuth(const std::string &token) : token(token) {
	}

	std::string GetAuthorizationHeader() override;

private:
	std::string token;
};

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/auth/bearer_token_auth.cpp`

```cpp
#include "calendar/auth/bearer_token_auth.hpp"

namespace duckdb {
namespace gcal {

std::string BearerTokenAuth::GetAuthorizationHeader() {
	return "Bearer " + token;
}

} // namespace gcal
} // namespace duckdb
```

#### 4. Auth factory (secret → provider dispatch)
**File**: `src/include/calendar/auth_factory.hpp`
**Changes**: `CreateAuthFromSecret` with the backward-compatible `secret_name=""` param (the Phase-5 cascade is already present in this end-state block — Phase 5's "MODIFY" is satisfied here; do not re-edit in Phase 5).

```cpp
#pragma once

#include "duckdb/main/client_context.hpp"

#include "calendar/auth/auth_provider.hpp"
#include "calendar/transport/http_client.hpp"

namespace duckdb {
namespace gcal {

// secret_name: when non-empty, resolve that named secret; otherwise use the default google_calendar secret.
std::unique_ptr<IAuthProvider> CreateAuthFromSecret(ClientContext &ctx, IHttpClient &http,
                                                    const std::string &secret_name = "");

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/auth_factory.cpp`
**Changes**: Provider dispatch factored into `BuildProvider`; default + named-secret resolution paths. Already includes the Phase-5 cascade (named secret via `GetSecretByName`).

```cpp
#include "calendar/auth_factory.hpp"

#include "calendar/util/secret.hpp"
#include "calendar/auth/bearer_token_auth.hpp"
#include "calendar/auth/service_account_auth.hpp"

#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {
namespace gcal {

static std::unique_ptr<IAuthProvider> BuildProvider(const KeyValueSecret &kv, IHttpClient &http) {
	auto provider = kv.GetProvider();
	if (provider == "key_file") {
		Value emailValue, keyValue;
		if (!kv.TryGetValue("email", emailValue)) {
			throw InvalidInputException("'email' not found in google_calendar secret");
		}
		if (!kv.TryGetValue("secret", keyValue)) {
			throw InvalidInputException("'secret' not found in google_calendar secret");
		}
		return make_uniq<ServiceAccountAuth>(http, emailValue.ToString(), keyValue.ToString());
	}
	Value tokenValue;
	if (!kv.TryGetValue("token", tokenValue)) {
		throw InvalidInputException("'token' not found in google_calendar secret");
	}
	return make_uniq<BearerTokenAuth>(tokenValue.ToString());
}

std::unique_ptr<IAuthProvider> CreateAuthFromSecret(ClientContext &ctx, IHttpClient &http,
                                                    const std::string &secret_name) {
	if (!secret_name.empty()) {
		auto &manager = SecretManager::Get(ctx);
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(ctx);
		auto entry = manager.GetSecretByName(transaction, secret_name);
		if (!entry || !entry->secret) {
			throw InvalidInputException("google_calendar secret '%s' not found", secret_name);
		}
		auto kv = dynamic_cast<const KeyValueSecret *>(entry->secret.get());
		if (!kv) {
			throw InvalidInputException("Secret '%s' is not a google_calendar secret", secret_name);
		}
		return BuildProvider(*kv, http);
	}

	auto match = GetSecretMatch(ctx, "google_calendar", "google_calendar");
	if (match.HasMatch()) {
		auto kv = dynamic_cast<const KeyValueSecret *>(&match.GetSecret());
		return BuildProvider(*kv, http);
	}
	return nullptr;
}

} // namespace gcal
} // namespace duckdb
```

#### 5. Secret-function registration + OAuth flow
**File**: `src/include/calendar_auth.hpp`

```cpp
#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

std::string InitiateOAuthFlow();

struct CreateGoogleCalendarSecretFunctions {
public:
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
```

**File**: `src/calendar_auth.cpp`

```cpp
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <random>
#include "json.hpp"

#include "duckdb/common/exception/binder_exception.hpp"

#include "calendar_auth.hpp"
#include "calendar/util/options.hpp"

using json = nlohmann::json;

namespace duckdb {

// Replaces gsheets_utils::generate_random_string (not ported).
static std::string GenerateRandomString(size_t length) {
	static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);
	std::string result;
	result.reserve(length);
	for (size_t i = 0; i < length; i++) {
		result += charset[dist(gen)];
	}
	return result;
}

static void CopySecret(const std::string &key, const CreateSecretInput &input, KeyValueSecret &result) {
	auto val = input.options.find(key);
	if (val != input.options.end()) {
		result.secret_map[key] = val->second;
	}
}

static void RegisterCommonSecretParameters(CreateSecretFunction &function) {
	function.named_parameters["token"] = LogicalType::VARCHAR;
}

static void RedactCommonKeys(KeyValueSecret &result) {
	result.redact_keys.insert("proxy_password");
}

static unique_ptr<BaseSecret> CreateSecretFromAccessToken(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	CopySecret("token", input, *result);
	RedactCommonKeys(*result);
	result->redact_keys.insert("token");
	return std::move(result);
}

static unique_ptr<BaseSecret> CreateSecretFromOAuth(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	string token = InitiateOAuthFlow();
	result->secret_map["token"] = token;
	RedactCommonKeys(*result);
	result->redact_keys.insert("token");
	return std::move(result);
}

static unique_ptr<BaseSecret> CreateSecretFromKeyFile(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);

	std::string email, secret;
	auto filepath = duckdb::gcal::GetStringOption(input.options, "filepath");
	if (filepath.empty()) {
		email = duckdb::gcal::GetStringOption(input.options, "email");
		if (email.empty()) {
			throw BinderException("Must provide email if not using filepath");
		}
		secret = duckdb::gcal::GetStringOption(input.options, "secret");
		if (secret.empty()) {
			throw BinderException("Must provide secret value if not using filepath");
		}
	} else {
		std::ifstream ifs(filepath);
		if (!ifs.is_open()) {
			throw IOException("Could not open JSON key file at: " + filepath);
		}
		json credentials_file = json::parse(ifs);
		email = credentials_file["client_email"].get<std::string>();
		secret = credentials_file["private_key"].get<std::string>();
	}

	(*result).secret_map["email"] = Value(email);
	(*result).secret_map["secret"] = Value(secret);
	CopySecret("filepath", input, *result); // Store the filepath anyway

	RedactCommonKeys(*result);
	result->redact_keys.insert("secret");
	result->redact_keys.insert("filepath");
	result->redact_keys.insert("token");

	return std::move(result);
}

void CreateGoogleCalendarSecretFunctions::Register(ExtensionLoader &loader) {
	string type = "google_calendar";

	SecretType secret_type;
	secret_type.name = type;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "oauth";

	CreateSecretFunction access_token_function = {type, "access_token", CreateSecretFromAccessToken, {}};
	access_token_function.named_parameters["access_token"] = LogicalType::VARCHAR;
	RegisterCommonSecretParameters(access_token_function);

	CreateSecretFunction oauth_function = {type, "oauth", CreateSecretFromOAuth, {}};
	oauth_function.named_parameters["use_oauth"] = LogicalType::BOOLEAN;
	RegisterCommonSecretParameters(oauth_function);

	CreateSecretFunction key_file_function = {type, "key_file", CreateSecretFromKeyFile, {}};
	key_file_function.named_parameters["filepath"] = LogicalType::VARCHAR;
	key_file_function.named_parameters["email"] = LogicalType::VARCHAR;
	key_file_function.named_parameters["secret"] = LogicalType::VARCHAR;
	RegisterCommonSecretParameters(key_file_function);

	loader.RegisterSecretType(secret_type);
	loader.RegisterFunction(access_token_function);
	loader.RegisterFunction(oauth_function);
	loader.RegisterFunction(key_file_function);
}

std::string InitiateOAuthFlow() {
	const char *client_id_env = std::getenv("GOOGLE_CALENDAR_OAUTH_CLIENT_ID");
	if (!client_id_env || !*client_id_env) {
		throw BinderException(
		    "The 'oauth' provider requires a registered Google OAuth client. Set GOOGLE_CALENDAR_OAUTH_CLIENT_ID "
		    "(and optionally GOOGLE_CALENDAR_OAUTH_REDIRECT_URI), or use the 'key_file' / 'access_token' provider.");
	}
	const char *redirect_env = std::getenv("GOOGLE_CALENDAR_OAUTH_REDIRECT_URI");
	const std::string client_id = client_id_env;
	const std::string redirect_uri = (redirect_env && *redirect_env) ? redirect_env : "urn:ietf:wg:oauth:2.0:oob";
	const std::string auth_url = "https://accounts.google.com/o/oauth2/v2/auth";

	std::string state = GenerateRandomString(10);
	std::string auth_request_url = auth_url + "?client_id=" + client_id + "&redirect_uri=" + redirect_uri +
	                               "&response_type=token" + "&scope=https://www.googleapis.com/auth/calendar" +
	                               "&state=" + state;

	std::cout << "Visit the below URL to authorize DuckDB Google Calendar" << '\n';
	std::cout << auth_request_url << '\n';

	bool should_open_browser = true;
#ifdef __linux__
	const char *display = std::getenv("DISPLAY");
	const char *wayland_display = std::getenv("WAYLAND_DISPLAY");
	if (!display && !wayland_display) {
		should_open_browser = false;
	}
#endif
	if (should_open_browser) {
#ifdef _WIN32
		system(("start \"\" \"" + auth_request_url + "\"").c_str());
#elif __APPLE__
		system(("open \"" + auth_request_url + "\"").c_str());
#elif __linux__
		system(("xdg-open \"" + auth_request_url + "\"").c_str());
#endif
	}
	std::cout << "After granting permission, enter the token: ";
	std::string access_token;
	std::cin >> access_token;
	return access_token;
}

} // namespace duckdb
```

#### 6. Wire secret registration into entry (MODIFY)
**File**: `src/google_calendar_extension.cpp`
**Changes**: Merge `CreateGoogleCalendarSecretFunctions::Register(loader)` (+ its include + OpenSSL init) into `LoadInternal`. **The full final code block lives in Phase 5** (design-annotated "Slice 5 state"); it already contains this secret-registration line plus the OpenSSL init. In this phase, add only the secret-registration call + `#include "calendar_auth.hpp"` (the storage-extension registration line stays out until Phase 5, where its symbol exists).

### Success Criteria:

#### Automated Verification:
- [x] JWT scope rebound: `grep -c 'auth/calendar' src/calendar/auth/service_account_auth.cpp` returns 1 and `grep -c spreadsheets src/calendar/auth/service_account_auth.cpp` returns 0
- [x] Secret type rebound: `grep -c '"google_calendar"' src/calendar/auth_factory.cpp` returns 1
- [x] No `gsheet` literals leaked: `grep -rl gsheet src/calendar src/calendar_auth.cpp 2>/dev/null | wc -l` returns 0 (reworded a stale comment from the plan's own fence that contained "gsheets")
- [x] Secret registration wired into entry: `grep -c 'CreateGoogleCalendarSecretFunctions::Register' src/google_calendar_extension.cpp` returns 1
- [x] Three providers registered: `grep -c '_function = {' src/calendar_auth.cpp` returns 3

#### Manual Verification:
- [ ] `CREATE SECRET ... (TYPE google_calendar, PROVIDER key_file, filepath '...')` resolves `client_email`/`private_key`
- [ ] `oauth` provider raises a clear `BinderException` when `GOOGLE_CALENDAR_OAUTH_CLIENT_ID` is unset (no hardcoded foreign client_id)
- [ ] Service-account JWT signs with `https://www.googleapis.com/auth/calendar`

---

## Phase 4: Calendar types + resources

### Overview
Add the calendar-list + event JSON types (scalars typed, nested as raw json), the URL-encoded query-string builder, the `BaseResource` (with net-new `DoDelete`), the `CalendarListResource` + `EventsResource`, and the `GoogleCalendarClient` facade (Calendar API base URL).

### Changes Required:

#### 1. Calendar JSON types
**File**: `src/include/calendar/types.hpp`

```cpp
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace duckdb {
namespace gcal {

// Calendar enumeration (attach-time). Event payloads are handled as raw nlohmann::json
// (VARCHAR passthrough, D6) in the scan (Slice 7) and DML (Slice 8), not typed here.
struct CalendarListEntry {
	std::string id = "";
	std::string summary = "";
	std::string description = "";
	std::string timeZone = "";
	bool primary = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CalendarListEntry, id, summary, description, timeZone, primary)

struct CalendarListResponse {
	std::vector<CalendarListEntry> items = {};
	std::string nextPageToken = "";
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CalendarListResponse, items, nextPageToken)

} // namespace gcal
} // namespace duckdb
```

#### 2. Query-string builder
**File**: `src/include/calendar/util/query.hpp`

```cpp
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace gcal {

std::string UrlEncode(const std::string &value);

class QueryBuilder {
public:
	QueryBuilder &Add(const std::string &key, const std::string &value);
	// Returns "" when empty, otherwise "?k1=v1&k2=v2" (percent-encoded).
	std::string Build() const;

private:
	std::vector<std::pair<std::string, std::string>> params;
};

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/util/query.cpp`

```cpp
#include <cctype>

#include "calendar/util/query.hpp"

namespace duckdb {
namespace gcal {

std::string UrlEncode(const std::string &value) {
	static const char hex[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(value.size() * 3);
	for (unsigned char c : value) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			result += static_cast<char>(c);
		} else {
			result += '%';
			result += hex[(c >> 4) & 0xF];
			result += hex[c & 0xF];
		}
	}
	return result;
}

QueryBuilder &QueryBuilder::Add(const std::string &key, const std::string &value) {
	params.emplace_back(key, value);
	return *this;
}

std::string QueryBuilder::Build() const {
	if (params.empty()) {
		return "";
	}
	std::string result = "?";
	bool first = true;
	for (const auto &p : params) {
		if (!first) {
			result += "&";
		}
		first = false;
		result += UrlEncode(p.first) + "=" + UrlEncode(p.second);
	}
	return result;
}

} // namespace gcal
} // namespace duckdb
```

#### 3. Base resource (+ DoDelete)
**File**: `src/include/calendar/resources/base.hpp`

```cpp
#pragma once
#include <string>

#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

class BaseResource {
protected:
	BaseResource(IHttpClient &http, const HttpHeaders &headers, const std::string &baseUrl)
	    : http(http), headers(headers), baseUrl(baseUrl) {};

	IHttpClient &http;
	const HttpHeaders &headers;
	std::string baseUrl;

	HttpResponse DoGet(const std::string &path);
	HttpResponse DoPost(const std::string &path, const std::string &body);
	HttpResponse DoPut(const std::string &path, const std::string &body);
	HttpResponse DoDelete(const std::string &path);
};

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/resources/base.cpp`

```cpp
#include "calendar/resources/base.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

HttpResponse BaseResource::DoGet(const std::string &path) {
	HttpRequest req;
	req.url = baseUrl + path;
	req.method = HttpMethod::GET;
	req.headers = headers;
	return http.Execute(req);
}

HttpResponse BaseResource::DoPost(const std::string &path, const std::string &body) {
	HttpRequest req;
	req.url = baseUrl + path;
	req.method = HttpMethod::POST;
	req.headers = headers;
	req.body = body;
	return http.Execute(req);
}

HttpResponse BaseResource::DoPut(const std::string &path, const std::string &body) {
	HttpRequest req;
	req.url = baseUrl + path;
	req.method = HttpMethod::PUT;
	req.headers = headers;
	req.body = body;
	return http.Execute(req);
}

HttpResponse BaseResource::DoDelete(const std::string &path) {
	HttpRequest req;
	req.url = baseUrl + path;
	req.method = HttpMethod::DEL;
	req.headers = headers;
	return http.Execute(req);
}

} // namespace gcal
} // namespace duckdb
```

#### 4. CalendarList resource
**File**: `src/include/calendar/resources/calendar_list.hpp`

```cpp
#pragma once

#include <string>

#include "calendar/resources/base.hpp"
#include "calendar/types.hpp"

namespace duckdb {
namespace gcal {

class CalendarListResource : protected BaseResource {
public:
	CalendarListResource(IHttpClient &http, const HttpHeaders &headers, const std::string &baseUrl)
	    : BaseResource(http, headers, baseUrl) {};

	CalendarListResponse List(const std::string &pageToken = "");
};

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/resources/calendar_list.cpp`

```cpp
#include "calendar/resources/calendar_list.hpp"
#include "calendar/util/response.hpp"
#include "calendar/util/query.hpp"
#include "calendar/types.hpp"

namespace duckdb {
namespace gcal {

CalendarListResponse CalendarListResource::List(const std::string &pageToken) {
	std::string path = "/users/me/calendarList";
	if (!pageToken.empty()) {
		path += "?pageToken=" + UrlEncode(pageToken);
	}
	return ParseResponse<CalendarListResponse>(DoGet(path));
}

} // namespace gcal
} // namespace duckdb
```

#### 5. Events resource
**File**: `src/include/calendar/resources/events.hpp`

```cpp
#pragma once

#include <string>

#include "json.hpp"

#include "calendar/resources/base.hpp"
#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

class EventsResource : protected BaseResource {
public:
	EventsResource(IHttpClient &http, const HttpHeaders &headers, const std::string &baseUrl,
	               const std::string &calendarId)
	    : BaseResource(http, headers, baseUrl), calendarId(calendarId) {};

	// queryString is the already-built "?k=v&..." suffix (or "").
	nlohmann::json List(const std::string &queryString);
	nlohmann::json Get(const std::string &eventId);
	nlohmann::json Insert(const nlohmann::json &event);
	nlohmann::json Update(const std::string &eventId, const nlohmann::json &event);
	void Delete(const std::string &eventId);

private:
	std::string calendarId;
};

} // namespace gcal
} // namespace duckdb
```

**File**: `src/calendar/resources/events.cpp`

```cpp
#include "json.hpp"

#include "calendar/resources/events.hpp"
#include "calendar/util/response.hpp"
#include "calendar/util/query.hpp"
#include "calendar/exception.hpp"

using json = nlohmann::json;

namespace duckdb {
namespace gcal {

json EventsResource::List(const std::string &queryString) {
	std::string path = "/calendars/" + UrlEncode(calendarId) + "/events" + queryString;
	return ParseResponse<json>(DoGet(path));
}

json EventsResource::Get(const std::string &eventId) {
	std::string path = "/calendars/" + UrlEncode(calendarId) + "/events/" + UrlEncode(eventId);
	return ParseResponse<json>(DoGet(path));
}

json EventsResource::Insert(const json &event) {
	std::string path = "/calendars/" + UrlEncode(calendarId) + "/events";
	return ParseResponse<json>(DoPost(path, event.dump()));
}

json EventsResource::Update(const std::string &eventId, const json &event) {
	std::string path = "/calendars/" + UrlEncode(calendarId) + "/events/" + UrlEncode(eventId);
	return ParseResponse<json>(DoPut(path, event.dump()));
}

void EventsResource::Delete(const std::string &eventId) {
	std::string path = "/calendars/" + UrlEncode(calendarId) + "/events/" + UrlEncode(eventId);
	auto response = DoDelete(path);
	// events.delete returns 204 No Content on success (200 also accepted defensively).
	if (response.statusCode != 200 && response.statusCode != 204) {
		throw CalendarApiException(response.statusCode, response.body);
	}
}

} // namespace gcal
} // namespace duckdb
```

#### 6. GoogleCalendarClient facade
**File**: `src/include/calendar/client.hpp`

```cpp
#pragma once

#include <string>

#include "calendar/util/version.hpp"

#include "calendar/auth/auth_provider.hpp"
#include "calendar/resources/calendar_list.hpp"
#include "calendar/resources/events.hpp"
#include "calendar/transport/http_client.hpp"
#include "calendar/transport/http_type.hpp"

namespace duckdb {
namespace gcal {

constexpr const char *DEFAULT_CALENDAR_API_URL = "https://www.googleapis.com/calendar/v3";

class GoogleCalendarClient {
public:
	GoogleCalendarClient(IHttpClient &http, IAuthProvider &auth,
	                     const std::string &baseUrl = DEFAULT_CALENDAR_API_URL)
	    : http(http), headers(BuildHeaders(auth)), baseUrl(baseUrl) {
	}

	CalendarListResource CalendarList() {
		return CalendarListResource(http, headers, baseUrl);
	}

	EventsResource Events(const std::string &calendarId) {
		return EventsResource(http, headers, baseUrl, calendarId);
	}

private:
	IHttpClient &http;
	HttpHeaders headers;
	std::string baseUrl;

	static HttpHeaders BuildHeaders(IAuthProvider &auth) {
		HttpHeaders h;
		h["Authorization"] = auth.GetAuthorizationHeader();
		h["Content-Type"] = "application/json";
		h["Accept"] = "application/json";

		std::string version = getVersion();
		h["User-Agent"] = "duckdb-google-calendar/" + (version.empty() ? std::string("dev") : version);

		return h;
	}
};
} // namespace gcal
} // namespace duckdb
```

### Success Criteria:

#### Automated Verification:
- [x] `DoDelete` added to base resource: `grep -c 'DoDelete' src/include/calendar/resources/base.hpp` returns 1
- [x] Calendar API base URL set: `grep -c 'calendar/v3' src/include/calendar/client.hpp` returns 1
- [x] calendarList endpoint: `grep -c 'users/me/calendarList' src/calendar/resources/calendar_list.cpp` returns 1
- [x] Events CRUD present: `grep -cE 'json EventsResource::(List|Get|Insert|Update)|void EventsResource::Delete' src/calendar/resources/events.cpp` returns 5
- [x] No forward-ref to storage layer: `grep -rn '"storage/' src/calendar src/include/calendar | wc -l` returns 0

#### Manual Verification:
- [ ] `GoogleCalendarClient.CalendarList().List()` deserializes calendarList `items` + `nextPageToken`
- [ ] Events `List`/`Get`/`Insert`/`Update` return raw json; `Delete` tolerates 204
- [ ] `calendarId`/`eventId` are URL-encoded (handle `@` and `:` in ids)

---

## Phase 5: StorageExtension + Catalog + Transaction(Manager)

### Overview
Net-new catalog backbone: the `CalendarStorageExtension` (attach + txn-manager function pointers), the `CalendarCatalog` (one schema; `Plan*` overrides), the no-op `CalendarTransactionManager`/`CalendarTransaction` (carrying the per-statement HTTP client + auth), and the `CalendarSchemaEntry` (lookup/scan; DDL throws). Register the storage extension and finalize the entry point.

### Changes Required:

#### 1. Storage extension
**File**: `src/include/storage/calendar_storage_extension.hpp`

```cpp
#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

class CalendarStorageExtension : public StorageExtension {
public:
	CalendarStorageExtension();
};

} // namespace duckdb
```

**File**: `src/storage/calendar_storage_extension.cpp`

```cpp
#include "storage/calendar_storage_extension.hpp"
#include "storage/calendar_catalog.hpp"
#include "storage/calendar_transaction_manager.hpp"

#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

static unique_ptr<Catalog> CalendarAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AttachOptions &options) {
	string secret_name;
	auto secret_entry = info.options.find("secret");
	if (secret_entry != info.options.end()) {
		secret_name = StringValue::Get(secret_entry->second);
	}

	auto catalog = make_uniq<CalendarCatalog>(db, info.path, secret_name);
	catalog->LoadCatalog(context);
	return std::move(catalog);
}

static unique_ptr<TransactionManager> CalendarCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                       AttachedDatabase &db, Catalog &catalog) {
	auto &calendar_catalog = catalog.Cast<CalendarCatalog>();
	return make_uniq<CalendarTransactionManager>(db, calendar_catalog);
}

CalendarStorageExtension::CalendarStorageExtension() {
	attach = CalendarAttach;
	create_transaction_manager = CalendarCreateTransactionManager;
}

} // namespace duckdb
```

#### 2. Catalog (header)
**File**: `src/include/storage/calendar_catalog.hpp`
**Changes**: `CalendarCatalog` overrides incl. `PlanMergeInto` (already declared in this end-state block — Phase 9's "MODIFY — declare PlanMergeInto" is satisfied here).

```cpp
#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {
class CalendarSchemaEntry;

class CalendarCatalog : public Catalog {
public:
	CalendarCatalog(AttachedDatabase &db, string path, string secret_name);
	~CalendarCatalog() override;

	const string &GetSecretName() const {
		return secret_name;
	}

	// Builds per-calendar tables via the Calendar API (enumeration body lands in Slice 6).
	void LoadCatalog(ClientContext &context);

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return "google_calendar";
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner, LogicalMergeInto &op,
	                                PhysicalOperator &plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override;
	string GetDBPath() override;

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	string path;
	string secret_name;
	unique_ptr<CalendarSchemaEntry> main_schema;
};

} // namespace duckdb
```

#### 3. Catalog (impl)
**File**: `src/storage/calendar_catalog.cpp`
**Changes**: This phase creates the catalog scaffold (ctor builds the single schema; `Initialize`/`LookupSchema`/`ScanSchemas`/size/path/DDL-reject). `LoadCatalog` enumeration arrives in Phase 6, `Plan*` bodies in Phase 8, `PlanMergeInto` in Phase 9, primary-naming in Phase 10. **The full final code block lives in Phase 6** (design-annotated "Slice 6 state", which the design renders as the complete file). Create the file here from that Phase 6 block; the later phases' deltas are already reflected in it. Phase 5's manual "stub error" checks reflect the design's progressive narrative, not this end-state code.

#### 4. Transaction manager (no-op)
**File**: `src/include/storage/calendar_transaction_manager.hpp`

```cpp
#pragma once

#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/common/mutex.hpp"

#include <vector>

namespace duckdb {
class CalendarCatalog;
class CalendarTransaction;

class CalendarTransactionManager : public TransactionManager {
public:
	CalendarTransactionManager(AttachedDatabase &db, CalendarCatalog &catalog);
	~CalendarTransactionManager() override;

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	CalendarCatalog &calendar_catalog;
	mutex transaction_lock;
	vector<unique_ptr<CalendarTransaction>> transactions;
};

} // namespace duckdb
```

**File**: `src/storage/calendar_transaction_manager.cpp`

```cpp
#include "storage/calendar_transaction_manager.hpp"
#include "storage/calendar_transaction.hpp"

namespace duckdb {

CalendarTransactionManager::CalendarTransactionManager(AttachedDatabase &db, CalendarCatalog &catalog)
    : TransactionManager(db), calendar_catalog(catalog) {
}

CalendarTransactionManager::~CalendarTransactionManager() = default;

Transaction &CalendarTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<CalendarTransaction>(calendar_catalog, *this, context);
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions.push_back(std::move(transaction));
	return result;
}

ErrorData CalendarTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	lock_guard<mutex> l(transaction_lock);
	for (idx_t i = 0; i < transactions.size(); i++) {
		if (transactions[i].get() == &transaction) {
			transactions.erase(transactions.begin() + i);
			break;
		}
	}
	return ErrorData();
}

void CalendarTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> l(transaction_lock);
	for (idx_t i = 0; i < transactions.size(); i++) {
		if (transactions[i].get() == &transaction) {
			transactions.erase(transactions.begin() + i);
			break;
		}
	}
}

void CalendarTransactionManager::Checkpoint(ClientContext &context, bool force) {
}

} // namespace duckdb
```

#### 5. Transaction (carries client + auth)
**File**: `src/include/storage/calendar_transaction.hpp`

```cpp
#pragma once

#include "duckdb/transaction/transaction.hpp"

#include "calendar/client.hpp"
#include "calendar/transport/http_client.hpp"
#include "calendar/auth/auth_provider.hpp"

#include <memory>

namespace duckdb {
class CalendarCatalog;

class CalendarTransaction : public Transaction {
public:
	CalendarTransaction(CalendarCatalog &catalog, TransactionManager &manager, ClientContext &context);
	~CalendarTransaction() override;

	static CalendarTransaction &Get(ClientContext &context, Catalog &catalog);

	// Lazily builds the per-statement HTTP client + auth from the catalog's secret.
	gcal::GoogleCalendarClient &GetClient(ClientContext &context);

private:
	CalendarCatalog &calendar_catalog;
	std::unique_ptr<gcal::IHttpClient> http;
	std::unique_ptr<gcal::IAuthProvider> auth;
	std::unique_ptr<gcal::GoogleCalendarClient> client;
};

} // namespace duckdb
```

**File**: `src/storage/calendar_transaction.cpp`

```cpp
#include "storage/calendar_transaction.hpp"
#include "storage/calendar_catalog.hpp"

#include "duckdb/common/exception.hpp"

#include "calendar/transport/client_factory.hpp"
#include "calendar/auth_factory.hpp"

namespace duckdb {

CalendarTransaction::CalendarTransaction(CalendarCatalog &catalog, TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), calendar_catalog(catalog) {
}

CalendarTransaction::~CalendarTransaction() = default;

CalendarTransaction &CalendarTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<CalendarTransaction>();
}

gcal::GoogleCalendarClient &CalendarTransaction::GetClient(ClientContext &context) {
	if (!client) {
		http = gcal::CreateHttpClient(context);
		auth = gcal::CreateAuthFromSecret(context, *http, calendar_catalog.GetSecretName());
		if (!auth) {
			throw InvalidInputException(
			    "No google_calendar secret found. Create one with CREATE SECRET (TYPE google_calendar, ...) "
			    "or pass SECRET <name> to ATTACH.");
		}
		client = make_uniq<gcal::GoogleCalendarClient>(*http, *auth);
	}
	return *client;
}

} // namespace duckdb
```

#### 6. Schema entry (lookup/scan; DDL throws)
**File**: `src/include/storage/calendar_schema_entry.hpp`

```cpp
#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {

class CalendarSchemaEntry : public SchemaCatalogEntry {
public:
	CalendarSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);

	// Called by attach-time enumeration (Slice 6) to register one events table per calendar.
	void AddTable(unique_ptr<CatalogEntry> table);

	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;

	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;

private:
	case_insensitive_map_t<unique_ptr<CatalogEntry>> tables;
};

} // namespace duckdb
```

**File**: `src/storage/calendar_schema_entry.cpp`

```cpp
#include "storage/calendar_schema_entry.hpp"

#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

CalendarSchemaEntry::CalendarSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info) {
}

void CalendarSchemaEntry::AddTable(unique_ptr<CatalogEntry> table) {
	tables[table->name] = std::move(table);
}

void CalendarSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	for (auto &entry : tables) {
		callback(*entry.second);
	}
}

void CalendarSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	for (auto &entry : tables) {
		callback(*entry.second);
	}
}

optional_ptr<CatalogEntry> CalendarSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                            const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto entry = tables.find(lookup_info.GetEntryName());
	if (entry == tables.end()) {
		return nullptr;
	}
	return entry->second.get();
}

static optional_ptr<CatalogEntry> RejectDDL() {
	throw NotImplementedException("google_calendar catalog is read/write for events only; DDL is not supported");
}

optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &,
                                                            TableCatalogEntry &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	return RejectDDL();
}
optional_ptr<CatalogEntry> CalendarSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	return RejectDDL();
}
void CalendarSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	throw NotImplementedException("google_calendar catalog is read/write for events only; DROP is not supported");
}
void CalendarSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	throw NotImplementedException("google_calendar catalog is read/write for events only; ALTER is not supported");
}

} // namespace duckdb
```

#### 7. Auth factory (MODIFY — already satisfied in Phase 3)
**File**: `src/include/calendar/auth_factory.hpp`, `src/calendar/auth_factory.cpp`
**Changes**: The `secret_name` cascade these phases require is already present in the Phase-3 end-state blocks. No further edit here — `CalendarCatalog::LoadCatalog` / `CalendarTransaction::GetClient` simply call `CreateAuthFromSecret(ctx, *http, secret_name)`.

#### 8. Entry point (MODIFY — full final code)
**File**: `src/google_calendar_extension.cpp`
**Changes**: Add the storage-extension registration (`StorageExtension::Register(config, "google_calendar", make_shared_ptr<CalendarStorageExtension>())` + its include) to `LoadInternal`. This is the design-annotated "Slice 5 state" — the full final code block:

```cpp
#define DUCKDB_EXTENSION_MAIN

#include "google_calendar_extension.hpp"
#include "duckdb.hpp"

#include "calendar_auth.hpp"
#include "storage/calendar_storage_extension.hpp"

#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	// Initialize OpenSSL (used for RS256 JWT signing in the service-account auth path).
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();

	// Register the google_calendar secret type + providers (oauth / access_token / key_file).
	CreateGoogleCalendarSecretFunctions::Register(loader);

	// Register the StorageExtension so ATTACH ... (TYPE google_calendar) dispatches to our catalog.
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	StorageExtension::Register(config, "google_calendar", make_shared_ptr<CalendarStorageExtension>());
}

void GoogleCalendarExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string GoogleCalendarExtension::Name() {
	return "google_calendar";
}

std::string GoogleCalendarExtension::Version() const {
#ifdef EXT_VERSION_GOOGLE_CALENDAR
	return EXT_VERSION_GOOGLE_CALENDAR;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(google_calendar, loader) {
	duckdb::LoadInternal(loader);
}
}
```

### Success Criteria:

#### Automated Verification:
- [x] Storage extension registered: `grep -c 'StorageExtension::Register(config, "google_calendar"' src/google_calendar_extension.cpp` returns 1
- [x] Catalog overrides all DML Plan* (concrete class): `grep -cE 'PhysicalOperator &CalendarCatalog::Plan(Insert|Delete|Update|CreateTableAs)' src/storage/calendar_catalog.cpp` returns 4
- [x] Named-secret param added: `grep -c 'const std::string &secret_name' src/include/calendar/auth_factory.hpp` returns 1
- [x] DDL rejected: `grep -c 'NotImplementedException' src/storage/calendar_schema_entry.cpp` >= 1
- [x] Manager has out-of-line dtor: `grep -c '~CalendarTransactionManager' src/storage/calendar_transaction_manager.cpp` returns 1

#### Manual Verification:
- [ ] `ATTACH 'me' AS cal (TYPE google_calendar, SECRET s)` succeeds; `SHOW TABLES FROM cal` returns 0 rows (tables arrive in Phase 6)
- [ ] `CREATE TABLE cal.x(...)` / `DROP` / `ALTER` error with the read/write-events-only message
- [ ] `INSERT/UPDATE/DELETE` on `cal.*` error with "implemented in a later slice" (replaced in Phase 8)

---

## Phase 6: Schema + table entries + enumeration

### Overview
Define the fixed `events` 15-column schema, the `CalendarTableEntry` (VARCHAR rowid, scan-function accessor), and the `LoadCatalog` enumeration that builds one `events` table per calendar with collision-safe naming.

### Changes Required:

#### 1. Fixed events schema
**File**: `src/include/storage/event_schema.hpp`

```cpp
#pragma once

#include "duckdb/parser/column_list.hpp"

namespace duckdb {

// Populates the fixed events schema (D6): scalar VARCHAR fields, start/end TIMESTAMP WITH TIME ZONE,
// all_day BOOLEAN, and attendees/recurrence/reminders/conference_data as VARCHAR raw-JSON passthrough.
void AddEventsColumns(ColumnList &columns);

} // namespace duckdb
```

**File**: `src/storage/event_schema.cpp`

```cpp
#include "storage/event_schema.hpp"

#include "duckdb/parser/column_definition.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

void AddEventsColumns(ColumnList &columns) {
	columns.AddColumn(ColumnDefinition("id", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("summary", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("description", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("location", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("status", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("html_link", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("created", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("updated", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("start", LogicalType::TIMESTAMP_TZ));
	columns.AddColumn(ColumnDefinition("end", LogicalType::TIMESTAMP_TZ));
	columns.AddColumn(ColumnDefinition("all_day", LogicalType::BOOLEAN));
	columns.AddColumn(ColumnDefinition("attendees", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("recurrence", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("reminders", LogicalType::VARCHAR));
	columns.AddColumn(ColumnDefinition("conference_data", LogicalType::VARCHAR));
}

} // namespace duckdb
```

#### 2. Table entry (VARCHAR rowid)
**File**: `src/include/storage/calendar_table_entry.hpp`

```cpp
#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class CalendarTableEntry : public TableCatalogEntry {
public:
	CalendarTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, string calendar_id);

	// The stable Google Calendar id (e.g. "primary" or "...@group.calendar.google.com").
	const string &GetCalendarId() const {
		return calendar_id;
	}

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	// Rowid = the event id (VARCHAR), not the conventional int64 row_t (D7).
	vector<column_t> GetRowIdColumns() const override;
	virtual_column_map_t GetVirtualColumns() const override;

private:
	string calendar_id;
};

} // namespace duckdb
```

**File**: `src/storage/calendar_table_entry.cpp`
**Changes**: Create the table entry. `GetScanFunction` is wired to the real scan in Phase 7. **The full final code block (with `GetScanFunction` wired) lives in Phase 7** (design-annotated "Slice 7 state"); the non-scan members (ctor, `GetStatistics`, `GetStorageInfo`, `GetRowIdColumns`, `GetVirtualColumns`) are stable from this phase. Create the file here from the Phase 7 block — its `GetScanFunction` body simply uses symbols (`MakeCalendarScanBindData`, `GetCalendarScanFunction`) that Phase 7 adds; the design provides no scan-less intermediate.

#### 3. Enumeration (MODIFY)
**File**: `src/storage/calendar_catalog.cpp`
**Changes**: `LoadCatalog` enumerates calendars (paginated `calendarList`), slugifies names (`primary` for the primary calendar — Phase 10 cascade), de-duplicates with a `_N` suffix, and adds one `CalendarTableEntry` per calendar. This is the design-annotated **"Slice 6 state"** — the full catalog implementation block. Phase 5 created the file from this same block (so the `Plan*` bodies / `PlanMergeInto` / primary-naming visible here are filled in Phases 8/9/10's narrative); the design provides no scan-less intermediate, so the file reaches this end state here.

```cpp
#include "storage/calendar_catalog.hpp"
#include "storage/calendar_schema_entry.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/event_schema.hpp"
#include "storage/calendar_insert.hpp"
#include "storage/calendar_update.hpp"
#include "storage/calendar_delete.hpp"

#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/operator/persistent/physical_merge_into.hpp"

#include "storage/calendar_merge.hpp"

#include "calendar/client.hpp"
#include "calendar/transport/client_factory.hpp"
#include "calendar/auth_factory.hpp"

#include <cctype>

namespace duckdb {

CalendarCatalog::CalendarCatalog(AttachedDatabase &db, string path, string secret_name)
    : Catalog(db), path(std::move(path)), secret_name(std::move(secret_name)) {
	CreateSchemaInfo schema_info;
	schema_info.schema = DEFAULT_SCHEMA;
	main_schema = make_uniq<CalendarSchemaEntry>(*this, schema_info);
}

CalendarCatalog::~CalendarCatalog() = default;

void CalendarCatalog::Initialize(bool load_builtin) {
	// Schema container is built in the constructor; per-calendar tables are added by LoadCatalog.
}

static string SlugifyName(const string &summary, const string &id) {
	const string &base = summary.empty() ? id : summary;
	string result;
	for (char ch : base) {
		auto c = static_cast<unsigned char>(ch);
		if (std::isalnum(c)) {
			result += static_cast<char>(std::tolower(c));
		} else if (ch == ' ' || ch == '-' || ch == '_') {
			result += '_';
		}
	}
	while (!result.empty() && result.front() == '_') {
		result.erase(result.begin());
	}
	while (!result.empty() && result.back() == '_') {
		result.pop_back();
	}
	if (result.empty()) {
		result = "calendar";
	}
	return result;
}

void CalendarCatalog::LoadCatalog(ClientContext &context) {
	auto http = gcal::CreateHttpClient(context);
	auto auth = gcal::CreateAuthFromSecret(context, *http, secret_name);
	if (!auth) {
		throw InvalidInputException(
		    "No google_calendar secret found. Create one with CREATE SECRET (TYPE google_calendar, ...) "
		    "or pass SECRET <name> to ATTACH.");
	}
	gcal::GoogleCalendarClient client(*http, *auth);

	case_insensitive_set_t used_names;
	string page_token;
	do {
		auto response = client.CalendarList().List(page_token);
		for (auto &cal : response.items) {
			// The primary calendar is always exposed as cal.primary (Slice 10 cascade).
			string base = cal.primary ? "primary" : SlugifyName(cal.summary, cal.id);
			string table_name = base;
			idx_t suffix = 2;
			while (used_names.find(table_name) != used_names.end()) {
				table_name = base + "_" + std::to_string(suffix++);
			}
			used_names.insert(table_name);

			CreateTableInfo info(*main_schema, table_name);
			AddEventsColumns(info.columns);
			auto entry = make_uniq<CalendarTableEntry>(*this, *main_schema, info, cal.id);
			main_schema->AddTable(std::move(entry));
		}
		page_token = response.nextPageToken;
	} while (!page_token.empty());
}

optional_ptr<CatalogEntry> CalendarCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	throw NotImplementedException("google_calendar catalog does not support CREATE SCHEMA");
}

optional_ptr<SchemaCatalogEntry> CalendarCatalog::LookupSchema(CatalogTransaction transaction,
                                                               const EntryLookupInfo &schema_lookup,
                                                               OnEntryNotFound if_not_found) {
	auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name.empty() || schema_name == DEFAULT_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	throw BinderException("Schema \"%s\" not found in google_calendar catalog", schema_name);
}

void CalendarCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

PhysicalOperator &CalendarCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                     LogicalCreateTable &op, PhysicalOperator &plan) {
	throw NotImplementedException("google_calendar catalog does not support CREATE TABLE AS");
}

PhysicalOperator &CalendarCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                              optional_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw NotImplementedException("google_calendar INSERT does not support RETURNING");
	}
	auto &table = op.table.Cast<CalendarTableEntry>();
	reference<PhysicalOperator> child = *plan;
	if (!op.column_index_map.empty()) {
		child = planner.ResolveDefaultsProjection(op, *plan);
	}
	auto &insert = planner.Make<CalendarInsert>(op.types, table, op.estimated_cardinality);
	insert.children.push_back(child);
	return insert;
}

PhysicalOperator &CalendarCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                              PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw NotImplementedException("google_calendar DELETE does not support RETURNING");
	}
	auto &table = op.table.Cast<CalendarTableEntry>();
	auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
	auto &del = planner.Make<CalendarDelete>(op.types, table, bound_ref.index, op.estimated_cardinality);
	del.children.push_back(plan);
	return del;
}

PhysicalOperator &CalendarCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                              PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw NotImplementedException("google_calendar UPDATE does not support RETURNING");
	}
	auto &table = op.table.Cast<CalendarTableEntry>();
	vector<idx_t> value_indices;
	for (auto &expr : op.expressions) {
		value_indices.push_back(expr->Cast<BoundReferenceExpression>().index);
	}
	auto &update =
	    planner.Make<CalendarUpdate>(op.types, table, op.columns, std::move(value_indices), op.estimated_cardinality);
	update.children.push_back(plan);
	return update;
}

PhysicalOperator &CalendarCatalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
                                                 LogicalMergeInto &op, PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw NotImplementedException("google_calendar MERGE does not support RETURNING");
	}
	auto &table = op.table.Cast<CalendarTableEntry>();

	map<MergeActionCondition, vector<unique_ptr<MergeIntoOperator>>> actions;
	for (auto &entry : op.actions) {
		vector<unique_ptr<MergeIntoOperator>> planned_actions;
		for (auto &action : entry.second) {
			planned_actions.push_back(PlanCalendarMergeIntoAction(context, op, planner, table, *action));
		}
		actions.emplace(entry.first, std::move(planned_actions));
	}

	auto &result = planner.Make<PhysicalMergeInto>(op.types, std::move(actions), op.row_id_start, op.source_marker,
	                                               false, op.return_chunk);
	result.children.push_back(plan);
	return result;
}

DatabaseSize CalendarCatalog::GetDatabaseSize(ClientContext &context) {
	return DatabaseSize();
}

bool CalendarCatalog::InMemory() {
	return false;
}

string CalendarCatalog::GetDBPath() {
	return path;
}

void CalendarCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw NotImplementedException("google_calendar catalog does not support DROP SCHEMA");
}

} // namespace duckdb
```

### Success Criteria:

#### Automated Verification:
- [x] 15 events columns: `grep -c 'columns.AddColumn' src/storage/event_schema.cpp` returns 15
- [x] `start`/`end` are TIMESTAMP_TZ: `grep -c 'LogicalType::TIMESTAMP_TZ' src/storage/event_schema.cpp` returns 2
- [x] VARCHAR rowid override: `grep -c 'TableColumn("rowid", LogicalType::VARCHAR)' src/storage/calendar_table_entry.cpp` returns 1
- [x] Enumeration calls calendarList: `grep -c 'client.CalendarList().List' src/storage/calendar_catalog.cpp` returns 1
- [x] Collision-safe naming: `grep -c 'used_names' src/storage/calendar_catalog.cpp` >= 2

#### Manual Verification:
- [ ] After ATTACH, `SHOW TABLES FROM cal` lists one table per calendar (slugged `summary`; `_2` suffix on collision)
- [ ] `DESCRIBE cal.<table>` shows the 15 columns with correct types (`start`/`end` TIMESTAMP WITH TIME ZONE)
- [ ] `SELECT FROM cal.<table>` errors "scan implemented in Slice 7" (stub replaced next slice)

---

## Phase 7: Scan TableFunction (pushdown + pagination)

### Overview
Implement the read path: a `TableFunction` with `pushdown_complex_filter` (extracts the widest `timeMin`/`timeMax` window from `start` predicates, retaining the exact residual), required-time-bound enforcement in `init_global`, paginated fetch (`nextPageToken`), and JSON→row mapping (VARCHAR passthrough, VARCHAR rowid). Wire it into `CalendarTableEntry::GetScanFunction`.

### Changes Required:

#### 1. Scan declaration
**File**: `src/include/storage/calendar_scan.hpp`

```cpp
#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

TableFunction GetCalendarScanFunction();

unique_ptr<FunctionData> MakeCalendarScanBindData(Catalog &catalog, string calendar_id, vector<string> names,
                                                  vector<LogicalType> types);

} // namespace duckdb
```

#### 2. Scan implementation
**File**: `src/storage/calendar_scan.cpp`

```cpp
#include "storage/calendar_scan.hpp"
#include "storage/calendar_transaction.hpp"

#include "json.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

#include "calendar/client.hpp"
#include "calendar/util/query.hpp"

#include <algorithm>

using json = nlohmann::json;

namespace duckdb {

static constexpr int64_t ONE_DAY_MICROS = 86400000000LL;

struct CalendarScanBindData : public TableFunctionData {
	CalendarScanBindData(Catalog &catalog, string calendar_id, vector<string> names, vector<LogicalType> types)
	    : catalog(catalog), calendar_id(std::move(calendar_id)), names(std::move(names)), types(std::move(types)) {
	}
	Catalog &catalog;
	string calendar_id;
	vector<string> names;
	vector<LogicalType> types;
	// Filled by pushdown_complex_filter (best-effort hint; residual guarantees exactness).
	string time_min;
	string time_max;
	bool has_lower = false;
	bool has_upper = false;
};

struct CalendarScanGlobalState : public GlobalTableFunctionState {
	gcal::GoogleCalendarClient *client = nullptr;
	string base_query;
	string next_page_token;
	json items = json::array();
	idx_t item_index = 0;
	bool finished = false;
	vector<column_t> column_ids;

	idx_t MaxThreads() const override {
		return 1;
	}
};

unique_ptr<FunctionData> MakeCalendarScanBindData(Catalog &catalog, string calendar_id, vector<string> names,
                                                  vector<LogicalType> types) {
	return make_uniq<CalendarScanBindData>(catalog, std::move(calendar_id), std::move(names), std::move(types));
}

// ---------- filter -> timeMin/timeMax extraction ----------

static ExpressionType FlipComparison(ExpressionType t) {
	switch (t) {
	case ExpressionType::COMPARE_GREATERTHAN:
		return ExpressionType::COMPARE_LESSTHAN;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ExpressionType::COMPARE_LESSTHANOREQUALTO;
	case ExpressionType::COMPARE_LESSTHAN:
		return ExpressionType::COMPARE_GREATERTHAN;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	default:
		return t;
	}
}

static string FormatWithBuffer(const Value &val, int64_t buffer_micros) {
	auto tstz = val.GetValue<timestamp_tz_t>();
	timestamp_t ts(tstz.value + buffer_micros);
	string s = Timestamp::ToString(ts);
	std::replace(s.begin(), s.end(), ' ', 'T');
	return s + "Z";
}

static void ExtractTimeBound(LogicalGet &get, Expression &expr, CalendarScanBindData &bind_data) {
	auto t = expr.GetExpressionType();
	if (t != ExpressionType::COMPARE_GREATERTHAN && t != ExpressionType::COMPARE_GREATERTHANOREQUALTO &&
	    t != ExpressionType::COMPARE_LESSTHAN && t != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
	    t != ExpressionType::COMPARE_EQUAL) {
		return;
	}
	auto &cmp = expr.Cast<BoundComparisonExpression>();
	auto &lhs = *cmp.left;
	auto &rhs = *cmp.right;

	Expression *col = nullptr;
	Expression *constant = nullptr;
	bool inverted = false;
	if (lhs.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF &&
	    rhs.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		col = &lhs;
		constant = &rhs;
	} else if (rhs.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF &&
	           lhs.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		col = &rhs;
		constant = &lhs;
		inverted = true;
	} else {
		return;
	}

	auto &cref = col->Cast<BoundColumnRefExpression>();
	auto &col_ids = get.GetColumnIds();
	if (cref.binding.column_index >= col_ids.size()) {
		return;
	}
	auto base_idx = col_ids[cref.binding.column_index].GetPrimaryIndex();
	if (base_idx >= get.names.size() || get.names[base_idx] != "start") {
		return;
	}

	auto &konst = constant->Cast<BoundConstantExpression>();
	if (konst.value.IsNull() || konst.value.type().id() != LogicalTypeId::TIMESTAMP_TZ) {
		return;
	}

	auto op = inverted ? FlipComparison(t) : t;
	if (op == ExpressionType::COMPARE_GREATERTHAN || op == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	    op == ExpressionType::COMPARE_EQUAL) {
		bind_data.time_min = FormatWithBuffer(konst.value, -ONE_DAY_MICROS);
		bind_data.has_lower = true;
	}
	if (op == ExpressionType::COMPARE_LESSTHAN || op == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	    op == ExpressionType::COMPARE_EQUAL) {
		bind_data.time_max = FormatWithBuffer(konst.value, ONE_DAY_MICROS);
		bind_data.has_upper = true;
	}
}

static void CalendarComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                                  vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<CalendarScanBindData>();
	for (auto &expr : filters) {
		ExtractTimeBound(get, *expr, bind_data);
	}
	// Retain ALL expressions: DuckDB keeps a LogicalFilter so the exact predicate is re-applied.
}

// ---------- fetch + json -> row mapping ----------

static void FetchPage(CalendarScanGlobalState &gstate, const CalendarScanBindData &bind_data) {
	string query = gstate.base_query;
	if (!gstate.next_page_token.empty()) {
		query += "&pageToken=" + gcal::UrlEncode(gstate.next_page_token);
	}
	auto response = gstate.client->Events(bind_data.calendar_id).List(query);
	gstate.next_page_token = response.value("nextPageToken", string());
	if (response.contains("items") && response["items"].is_array()) {
		gstate.items = response["items"];
	} else {
		gstate.items = json::array();
	}
	gstate.item_index = 0;
}

static Value JsonString(const json &event, const char *key) {
	if (!event.contains(key) || event[key].is_null()) {
		return Value(LogicalType::VARCHAR);
	}
	if (event[key].is_string()) {
		return Value(event[key].get<string>());
	}
	return Value(event[key].dump());
}

static Value JsonRaw(const json &event, const char *key) {
	if (!event.contains(key) || event[key].is_null()) {
		return Value(LogicalType::VARCHAR);
	}
	return Value(event[key].dump());
}

static bool IsAllDay(const json &event) {
	return event.contains("start") && event["start"].is_object() && event["start"].contains("date") &&
	       !event["start"].contains("dateTime");
}

static Value ParseEventTime(const json &event, const char *which) {
	if (!event.contains(which) || !event[which].is_object()) {
		return Value(LogicalType::TIMESTAMP_TZ);
	}
	const auto &node = event[which];
	string raw;
	if (node.contains("dateTime") && node["dateTime"].is_string()) {
		raw = node["dateTime"].get<string>();
	} else if (node.contains("date") && node["date"].is_string()) {
		raw = node["date"].get<string>();
	} else {
		return Value(LogicalType::TIMESTAMP_TZ);
	}
	Value out;
	string err;
	if (Value(raw).DefaultTryCastAs(LogicalType::TIMESTAMP_TZ, out, &err)) {
		return out;
	}
	return Value(LogicalType::TIMESTAMP_TZ);
}

static Value ExtractField(const json &event, const string &field) {
	if (field == "id") {
		return JsonString(event, "id");
	}
	if (field == "summary") {
		return JsonString(event, "summary");
	}
	if (field == "description") {
		return JsonString(event, "description");
	}
	if (field == "location") {
		return JsonString(event, "location");
	}
	if (field == "status") {
		return JsonString(event, "status");
	}
	if (field == "html_link") {
		return JsonString(event, "htmlLink");
	}
	if (field == "created") {
		return JsonString(event, "created");
	}
	if (field == "updated") {
		return JsonString(event, "updated");
	}
	if (field == "start") {
		return ParseEventTime(event, "start");
	}
	if (field == "end") {
		return ParseEventTime(event, "end");
	}
	if (field == "all_day") {
		return Value::BOOLEAN(IsAllDay(event));
	}
	if (field == "attendees") {
		return JsonRaw(event, "attendees");
	}
	if (field == "recurrence") {
		return JsonRaw(event, "recurrence");
	}
	if (field == "reminders") {
		return JsonRaw(event, "reminders");
	}
	if (field == "conference_data") {
		return JsonRaw(event, "conferenceData");
	}
	return Value();
}

static unique_ptr<GlobalTableFunctionState> CalendarScanInitGlobal(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<CalendarScanBindData>();
	if (!bind_data.has_lower && !bind_data.has_upper) {
		throw BinderException(
		    "google_calendar scan requires an explicit time bound on \"start\" (e.g. "
		    "WHERE start >= TIMESTAMPTZ '2026-06-01 00:00+00' AND start < TIMESTAMPTZ '2026-07-01 00:00+00')");
	}
	auto state = make_uniq<CalendarScanGlobalState>();
	state->column_ids = input.column_ids;

	gcal::QueryBuilder qb;
	qb.Add("singleEvents", "true").Add("orderBy", "startTime").Add("maxResults", "2500");
	if (bind_data.has_lower) {
		qb.Add("timeMin", bind_data.time_min);
	}
	if (bind_data.has_upper) {
		qb.Add("timeMax", bind_data.time_max);
	}
	state->base_query = qb.Build();

	auto &transaction = CalendarTransaction::Get(context, bind_data.catalog);
	state->client = &transaction.GetClient(context);

	FetchPage(*state, bind_data);
	return std::move(state);
}

static void CalendarScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<CalendarScanBindData>();
	auto &gstate = data_p.global_state->Cast<CalendarScanGlobalState>();

	idx_t out_idx = 0;
	while (out_idx < STANDARD_VECTOR_SIZE && !gstate.finished) {
		if (gstate.item_index >= gstate.items.size()) {
			if (gstate.next_page_token.empty()) {
				gstate.finished = true;
				break;
			}
			FetchPage(gstate, bind_data);
			continue;
		}
		const auto &event = gstate.items[gstate.item_index++];
		for (idx_t col = 0; col < output.ColumnCount(); col++) {
			column_t cid = gstate.column_ids[col];
			if (cid == COLUMN_IDENTIFIER_ROW_ID) {
				output.SetValue(col, out_idx, JsonString(event, "id"));
			} else {
				output.SetValue(col, out_idx, ExtractField(event, bind_data.names[cid]));
			}
		}
		out_idx++;
	}
	output.SetCardinality(out_idx);
}

TableFunction GetCalendarScanFunction() {
	TableFunction function("google_calendar_scan", {}, CalendarScan, nullptr, CalendarScanInitGlobal);
	function.pushdown_complex_filter = CalendarComplexFilter;
	function.projection_pushdown = true;
	return function;
}

} // namespace duckdb
```

#### 3. Wire GetScanFunction (MODIFY)
**File**: `src/storage/calendar_table_entry.cpp`
**Changes**: `GetScanFunction` builds bind data via `MakeCalendarScanBindData` and returns `GetCalendarScanFunction()`. **This is the design-annotated "Slice 7 state" — the full final code block** (already used to create the file in Phase 6):

```cpp
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_scan.hpp"

#include "duckdb/common/table_column.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

CalendarTableEntry::CalendarTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                       string calendar_id)
    : TableCatalogEntry(catalog, schema, info), calendar_id(std::move(calendar_id)) {
}

unique_ptr<BaseStatistics> CalendarTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction CalendarTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	vector<string> names;
	vector<LogicalType> types;
	for (auto &col : GetColumns().Logical()) {
		names.push_back(col.Name());
		types.push_back(col.Type());
	}
	bind_data = MakeCalendarScanBindData(ParentCatalog(), calendar_id, std::move(names), std::move(types));
	return GetCalendarScanFunction();
}

TableStorageInfo CalendarTableEntry::GetStorageInfo(ClientContext &context) {
	return TableStorageInfo();
}

vector<column_t> CalendarTableEntry::GetRowIdColumns() const {
	vector<column_t> result;
	result.push_back(COLUMN_IDENTIFIER_ROW_ID);
	return result;
}

virtual_column_map_t CalendarTableEntry::GetVirtualColumns() const {
	virtual_column_map_t virtual_columns;
	virtual_columns.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::VARCHAR)));
	return virtual_columns;
}

} // namespace duckdb
```

### Success Criteria:

#### Automated Verification:
- [x] Scan registers pushdown + projection: `grep -cE 'function.pushdown_complex_filter|function.projection_pushdown = true' src/storage/calendar_scan.cpp` returns 2
- [x] Required time bound enforced: `grep -c 'requires an explicit time bound' src/storage/calendar_scan.cpp` returns 1
- [x] Residual retained (callback never erases): `grep -c 'filters.erase\|filters.clear' src/storage/calendar_scan.cpp` returns 0
- [x] Pagination loop: `grep -c 'next_page_token' src/storage/calendar_scan.cpp` >= 3
- [x] Rowid emission: `grep -c 'COLUMN_IDENTIFIER_ROW_ID' src/storage/calendar_scan.cpp` returns 1
- [x] GetScanFunction wired: `grep -c 'GetCalendarScanFunction()' src/storage/calendar_table_entry.cpp` returns 1

#### Manual Verification:
- [ ] `SELECT ... WHERE start >= A AND start < B` issues a GET with `timeMin`/`timeMax` + `singleEvents=true&orderBy=startTime`; results exact (residual filter applied)
- [ ] `SELECT * FROM cal.x` (no time bound) errors "requires an explicit time bound"
- [ ] Multi-page fixture (`nextPageToken`) returns all rows concatenated
- [ ] DELETE/UPDATE path: scan emits the event `id` as the VARCHAR rowid column

---

## Phase 8: DML operators (Insert/Update/Delete)

### Overview
Implement the write path: header-only row↔event JSON mapping helpers plus three custom `EXTENSION` `PhysicalOperator` sinks — INSERT (POST per row), UPDATE (read-modify-write GET+PUT per row), DELETE (DELETE per row, VARCHAR rowid) — all `ParallelSink()==false`. Fill the catalog `Plan*` bodies.

### Changes Required:

#### 1. Row↔event mapping helpers (header-only)
**File**: `src/include/storage/event_mapping.hpp`

```cpp
#pragma once

#include "json.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>

namespace duckdb {
namespace gcal_map {

// events-schema column order (must match AddEventsColumns / Slice 6):
// 0 id, 1 summary, 2 description, 3 location, 4 status, 5 html_link, 6 created, 7 updated,
// 8 start, 9 end, 10 all_day, 11 attendees, 12 recurrence, 13 reminders, 14 conference_data

inline string FormatRfc3339(const Value &val) {
	auto tstz = val.GetValue<timestamp_tz_t>();
	timestamp_t ts(tstz.value);
	string s = Timestamp::ToString(ts);
	std::replace(s.begin(), s.end(), ' ', 'T');
	return s + "Z";
}

inline string FormatDate(const Value &val) {
	auto tstz = val.GetValue<timestamp_tz_t>();
	timestamp_t ts(tstz.value);
	string s = Timestamp::ToString(ts);
	return s.substr(0, 10);
}

inline nlohmann::json TimeNode(const Value &val, bool all_day) {
	nlohmann::json node;
	if (all_day) {
		node["date"] = FormatDate(val);
	} else {
		node["dateTime"] = FormatRfc3339(val);
	}
	return node;
}

inline void SetJsonPassthrough(nlohmann::json &event, const char *key, const Value &val) {
	if (val.IsNull()) {
		return;
	}
	auto text = val.ToString();
	if (text.empty()) {
		return;
	}
	try {
		event[key] = nlohmann::json::parse(text);
	} catch (...) {
		// best-effort: ignore malformed passthrough JSON
	}
}

inline bool ExistingAllDay(const nlohmann::json &event) {
	return event.contains("start") && event["start"].is_object() && event["start"].contains("date") &&
	       !event["start"].contains("dateTime");
}

// Build a full event body from an INSERT row (all 15 columns present, schema order).
inline nlohmann::json RowToEvent(DataChunk &chunk, idx_t row) {
	nlohmann::json event;
	auto set_str = [&](const char *key, idx_t col) {
		auto v = chunk.GetValue(col, row);
		if (!v.IsNull()) {
			event[key] = v.ToString();
		}
	};
	set_str("summary", 1);
	set_str("description", 2);
	set_str("location", 3);
	set_str("status", 4);

	auto all_day_v = chunk.GetValue(10, row);
	bool all_day = !all_day_v.IsNull() && BooleanValue::Get(all_day_v);
	auto start_v = chunk.GetValue(8, row);
	auto end_v = chunk.GetValue(9, row);
	if (!start_v.IsNull()) {
		event["start"] = TimeNode(start_v, all_day);
	}
	if (!end_v.IsNull()) {
		event["end"] = TimeNode(end_v, all_day);
	}

	SetJsonPassthrough(event, "attendees", chunk.GetValue(11, row));
	SetJsonPassthrough(event, "recurrence", chunk.GetValue(12, row));
	SetJsonPassthrough(event, "reminders", chunk.GetValue(13, row));
	SetJsonPassthrough(event, "conferenceData", chunk.GetValue(14, row));
	return event;
}

// Re-encode an existing start/end node when only all_day flips.
inline void ReencodeTimeNode(nlohmann::json &event, const char *key, bool all_day) {
	if (!event.contains(key) || !event[key].is_object()) {
		return;
	}
	auto &node = event[key];
	string raw;
	if (node.contains("dateTime") && node["dateTime"].is_string()) {
		raw = node["dateTime"].get<string>();
	} else if (node.contains("date") && node["date"].is_string()) {
		raw = node["date"].get<string>();
	} else {
		return;
	}
	nlohmann::json rebuilt;
	if (all_day) {
		rebuilt["date"] = raw.substr(0, 10);
	} else {
		rebuilt["dateTime"] = raw.size() == 10 ? (raw + "T00:00:00Z") : raw;
	}
	event[key] = rebuilt;
}

// Apply one SET column (by schema index) to an existing event during UPDATE.
inline void ApplySet(nlohmann::json &event, idx_t schema_index, const Value &val, bool all_day) {
	auto set_or_remove = [&](const char *key) {
		if (val.IsNull()) {
			event.erase(key);
		} else {
			event[key] = val.ToString();
		}
	};
	switch (schema_index) {
	case 1:
		set_or_remove("summary");
		break;
	case 2:
		set_or_remove("description");
		break;
	case 3:
		set_or_remove("location");
		break;
	case 4:
		set_or_remove("status");
		break;
	case 8:
		if (val.IsNull()) {
			event.erase("start");
		} else {
			event["start"] = TimeNode(val, all_day);
		}
		break;
	case 9:
		if (val.IsNull()) {
			event.erase("end");
		} else {
			event["end"] = TimeNode(val, all_day);
		}
		break;
	case 10:
		// all_day handled via the all_day flag; re-encode existing nodes if start/end were not also SET.
		ReencodeTimeNode(event, "start", all_day);
		ReencodeTimeNode(event, "end", all_day);
		break;
	case 11:
		SetJsonPassthrough(event, "attendees", val);
		break;
	case 12:
		SetJsonPassthrough(event, "recurrence", val);
		break;
	case 13:
		SetJsonPassthrough(event, "reminders", val);
		break;
	case 14:
		SetJsonPassthrough(event, "conferenceData", val);
		break;
	default:
		// 0 id, 5 html_link, 6 created, 7 updated are server-managed
		throw InvalidInputException("google_calendar: column at index %llu is read-only and cannot be updated",
		                            (unsigned long long)schema_index);
	}
}

} // namespace gcal_map
} // namespace duckdb
```

#### 2. INSERT operator
**File**: `src/include/storage/calendar_insert.hpp`

```cpp
#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class CalendarTableEntry;

class CalendarInsert : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	CalendarInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
	               idx_t estimated_cardinality);

	CalendarTableEntry &table;

public:
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
};

} // namespace duckdb
```

**File**: `src/storage/calendar_insert.cpp`

```cpp
#include "storage/calendar_insert.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_transaction.hpp"
#include "storage/event_mapping.hpp"

#include "calendar/client.hpp"

namespace duckdb {

CalendarInsert::CalendarInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
                               idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table) {
}

class CalendarInsertGlobalSinkState : public GlobalSinkState {
public:
	gcal::GoogleCalendarClient *client = nullptr;
	string calendar_id;
	idx_t insert_count = 0;
};

unique_ptr<GlobalSinkState> CalendarInsert::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<CalendarInsertGlobalSinkState>();
	auto &transaction = CalendarTransaction::Get(context, table.ParentCatalog());
	state->client = &transaction.GetClient(context);
	state->calendar_id = table.GetCalendarId();
	return std::move(state);
}

SinkResultType CalendarInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CalendarInsertGlobalSinkState>();
	chunk.Flatten();
	for (idx_t row = 0; row < chunk.size(); row++) {
		auto event = gcal_map::RowToEvent(chunk, row);
		gstate.client->Events(gstate.calendar_id).Insert(event);
		gstate.insert_count++;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

class CalendarInsertSourceState : public GlobalSourceState {
public:
	bool finished = false;
};

unique_ptr<GlobalSourceState> CalendarInsert::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CalendarInsertSourceState>();
}

SourceResultType CalendarInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CalendarInsertSourceState>();
	if (state.finished) {
		return SourceResultType::FINISHED;
	}
	auto &sink = sink_state->Cast<CalendarInsertGlobalSinkState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(sink.insert_count)));
	state.finished = true;
	return SourceResultType::FINISHED;
}

} // namespace duckdb
```

#### 3. UPDATE operator
**File**: `src/include/storage/calendar_update.hpp`

```cpp
#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/index_vector.hpp"

namespace duckdb {
class CalendarTableEntry;

class CalendarUpdate : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	CalendarUpdate(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
	               vector<PhysicalIndex> columns, vector<idx_t> value_indices, idx_t estimated_cardinality);

	CalendarTableEntry &table;
	vector<PhysicalIndex> columns;     // SET target schema columns
	vector<idx_t> value_indices;       // parallel: child-chunk index of each SET value

public:
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
};

} // namespace duckdb
```

**File**: `src/storage/calendar_update.cpp`

```cpp
#include "storage/calendar_update.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_transaction.hpp"
#include "storage/event_mapping.hpp"

#include "calendar/client.hpp"

#include "json.hpp"

using json = nlohmann::json;

namespace duckdb {

CalendarUpdate::CalendarUpdate(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
                               vector<PhysicalIndex> columns, vector<idx_t> value_indices,
                               idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table), columns(std::move(columns)), value_indices(std::move(value_indices)) {
}

class CalendarUpdateGlobalSinkState : public GlobalSinkState {
public:
	gcal::GoogleCalendarClient *client = nullptr;
	string calendar_id;
	idx_t update_count = 0;
};

unique_ptr<GlobalSinkState> CalendarUpdate::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<CalendarUpdateGlobalSinkState>();
	auto &transaction = CalendarTransaction::Get(context, table.ParentCatalog());
	state->client = &transaction.GetClient(context);
	state->calendar_id = table.GetCalendarId();
	return std::move(state);
}

SinkResultType CalendarUpdate::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CalendarUpdateGlobalSinkState>();
	chunk.Flatten();
	// Child chunk layout (core PhysicalUpdate): SET values at value_indices[c], rowid at the last column.
	idx_t row_id_index = chunk.ColumnCount() - 1;
	for (idx_t row = 0; row < chunk.size(); row++) {
		auto id_val = chunk.GetValue(row_id_index, row);
		if (id_val.IsNull()) {
			continue;
		}
		string id = id_val.ToString();
		json event = gstate.client->Events(gstate.calendar_id).Get(id);

		bool all_day = gcal_map::ExistingAllDay(event);
		for (idx_t c = 0; c < columns.size(); c++) {
			if (columns[c].index == 10) {
				auto v = chunk.GetValue(value_indices[c], row);
				all_day = !v.IsNull() && BooleanValue::Get(v);
			}
		}
		for (idx_t c = 0; c < columns.size(); c++) {
			gcal_map::ApplySet(event, columns[c].index, chunk.GetValue(value_indices[c], row), all_day);
		}
		gstate.client->Events(gstate.calendar_id).Update(id, event);
		gstate.update_count++;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

class CalendarUpdateSourceState : public GlobalSourceState {
public:
	bool finished = false;
};

unique_ptr<GlobalSourceState> CalendarUpdate::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CalendarUpdateSourceState>();
}

SourceResultType CalendarUpdate::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CalendarUpdateSourceState>();
	if (state.finished) {
		return SourceResultType::FINISHED;
	}
	auto &sink = sink_state->Cast<CalendarUpdateGlobalSinkState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(sink.update_count)));
	state.finished = true;
	return SourceResultType::FINISHED;
}

} // namespace duckdb
```

#### 4. DELETE operator
**File**: `src/include/storage/calendar_delete.hpp`

```cpp
#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class CalendarTableEntry;

class CalendarDelete : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	CalendarDelete(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
	               idx_t row_id_index, idx_t estimated_cardinality);

	CalendarTableEntry &table;
	idx_t row_id_index;

public:
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
};

} // namespace duckdb
```

**File**: `src/storage/calendar_delete.cpp`

```cpp
#include "storage/calendar_delete.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_transaction.hpp"

#include "calendar/client.hpp"

namespace duckdb {

CalendarDelete::CalendarDelete(PhysicalPlan &physical_plan, vector<LogicalType> types, CalendarTableEntry &table,
                               idx_t row_id_index, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table), row_id_index(row_id_index) {
}

class CalendarDeleteGlobalSinkState : public GlobalSinkState {
public:
	gcal::GoogleCalendarClient *client = nullptr;
	string calendar_id;
	idx_t delete_count = 0;
};

unique_ptr<GlobalSinkState> CalendarDelete::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<CalendarDeleteGlobalSinkState>();
	auto &transaction = CalendarTransaction::Get(context, table.ParentCatalog());
	state->client = &transaction.GetClient(context);
	state->calendar_id = table.GetCalendarId();
	return std::move(state);
}

SinkResultType CalendarDelete::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CalendarDeleteGlobalSinkState>();
	chunk.Flatten();
	for (idx_t row = 0; row < chunk.size(); row++) {
		auto id_val = chunk.GetValue(row_id_index, row);
		if (id_val.IsNull()) {
			continue;
		}
		gstate.client->Events(gstate.calendar_id).Delete(id_val.ToString());
		gstate.delete_count++;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

class CalendarDeleteSourceState : public GlobalSourceState {
public:
	bool finished = false;
};

unique_ptr<GlobalSourceState> CalendarDelete::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CalendarDeleteSourceState>();
}

SourceResultType CalendarDelete::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<CalendarDeleteSourceState>();
	if (state.finished) {
		return SourceResultType::FINISHED;
	}
	auto &sink = sink_state->Cast<CalendarDeleteGlobalSinkState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(sink.delete_count)));
	state.finished = true;
	return SourceResultType::FINISHED;
}

} // namespace duckdb
```

#### 5. Catalog Plan* bodies (MODIFY)
**File**: `src/storage/calendar_catalog.cpp`
**Changes**: `PlanInsert`/`PlanUpdate`/`PlanDelete` replace their stubs with `planner.Make<Calendar{Insert,Update,Delete}>(...)` + child push, plus the DML-operator includes. **Already present in the full catalog code block** (created in Phase 5 from the design's end-state "Slice 6 state" snapshot, which renders the complete `Plan*` bodies). If Phase 5 created the file from that block, the bodies are already in place; otherwise apply them from the catalog code block in Phase 5/6.

### Success Criteria:

#### Automated Verification:
- [x] No DML stub throws remain: `grep -c 'implemented in a later slice' src/storage/calendar_catalog.cpp` returns 0
- [x] Operators are EXTENSION type: `grep -cl 'PhysicalOperatorType::EXTENSION' src/storage/calendar_insert.cpp src/storage/calendar_update.cpp src/storage/calendar_delete.cpp` returns 3
- [x] Serial sink: `grep -c 'return false' src/include/storage/calendar_insert.hpp` >= 1 (`ParallelSink`)
- [x] UPDATE is read-modify-write: `grep -c 'Get(id)' src/storage/calendar_update.cpp` returns 1 and `grep -c 'Update(id, event)' src/storage/calendar_update.cpp` returns 1
- [x] DELETE reads rowid: `grep -c 'row_id_index' src/storage/calendar_delete.cpp` >= 2

#### Manual Verification:
- [ ] `INSERT INTO cal.x (summary,start,"end") VALUES (...)` POSTs one events.insert per row; reports N
- [ ] `DELETE FROM cal.x WHERE id='...'` issues events.delete; reports N
- [ ] `UPDATE cal.x SET location='...' WHERE id='...'` does GET then PUT (read-modify-write); reports N
- [ ] Updating `id`/`html_link`/`created`/`updated` errors (read-only); DML serialized (ParallelSink false)

---

## Phase 9: MERGE (PlanMergeInto)

### Overview
Override the non-pure `PlanMergeInto` to compose synthetic per-action operators into core `PhysicalMergeInto`, reusing the Calendar{Insert,Update,Delete} EXTENSION sinks via substitution (no `Cast<PhysicalUpdate>`).

### Changes Required:

#### 1. MERGE planning helper
**File**: `src/include/storage/calendar_merge.hpp`

```cpp
#pragma once

#include "duckdb/common/helper.hpp"

namespace duckdb {
class ClientContext;
class PhysicalPlanGenerator;
class LogicalMergeInto;
class BoundMergeIntoAction;
class MergeIntoOperator;
class CalendarTableEntry;

unique_ptr<MergeIntoOperator> PlanCalendarMergeIntoAction(ClientContext &context, LogicalMergeInto &op,
                                                          PhysicalPlanGenerator &planner, CalendarTableEntry &table,
                                                          BoundMergeIntoAction &action);

} // namespace duckdb
```

**File**: `src/storage/calendar_merge.cpp`

```cpp
#include "storage/calendar_merge.hpp"
#include "storage/calendar_table_entry.hpp"
#include "storage/calendar_insert.hpp"
#include "storage/calendar_update.hpp"
#include "storage/calendar_delete.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/operator/persistent/physical_merge_into.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

unique_ptr<MergeIntoOperator> PlanCalendarMergeIntoAction(ClientContext &context, LogicalMergeInto &op,
                                                          PhysicalPlanGenerator &planner, CalendarTableEntry &table,
                                                          BoundMergeIntoAction &action) {
	auto result = make_uniq<MergeIntoOperator>();
	result->action_type = action.action_type;
	result->condition = std::move(action.condition);

	auto return_types = op.types;
	if (op.return_chunk) {
		return_types.pop_back();
	}
	auto cardinality = op.EstimateCardinality(context);

	switch (action.action_type) {
	case MergeActionType::MERGE_UPDATE: {
		vector<idx_t> value_indices;
		for (auto &expr : action.expressions) {
			value_indices.push_back(expr->Cast<BoundReferenceExpression>().index);
		}
		result->op = planner.Make<CalendarUpdate>(return_types, table, std::move(action.columns),
		                                          std::move(value_indices), cardinality);
		break;
	}
	case MergeActionType::MERGE_DELETE: {
		result->op = planner.Make<CalendarDelete>(return_types, table, op.row_id_start, cardinality);
		break;
	}
	case MergeActionType::MERGE_INSERT: {
		result->op = planner.Make<CalendarInsert>(return_types, table, cardinality);
		if (!action.column_index_map.empty()) {
			vector<unique_ptr<Expression>> new_expressions;
			for (auto &col : op.table.GetColumns().Physical()) {
				auto storage_idx = col.StorageOid();
				auto mapped_index = action.column_index_map[col.Physical()];
				if (mapped_index == DConstants::INVALID_INDEX) {
					new_expressions.push_back(op.bound_defaults[storage_idx]->Copy());
				} else {
					new_expressions.push_back(std::move(action.expressions[mapped_index]));
				}
			}
			action.expressions = std::move(new_expressions);
		}
		result->expressions = std::move(action.expressions);
		break;
	}
	case MergeActionType::MERGE_ERROR:
		result->expressions = std::move(action.expressions);
		break;
	case MergeActionType::MERGE_DO_NOTHING:
		break;
	default:
		throw InternalException("Unsupported merge action in google_calendar catalog");
	}
	return result;
}

} // namespace duckdb
```

#### 2. Catalog PlanMergeInto (MODIFY)
**File**: `src/include/storage/calendar_catalog.hpp`
**Changes**: Declare `PlanMergeInto` — **already declared** in the Phase-5 end-state header block. No further edit.

**File**: `src/storage/calendar_catalog.cpp`
**Changes**: Define `PlanMergeInto` (composes per-action ops into `planner.Make<PhysicalMergeInto>`) + the merge/physical_merge_into includes. **Already present** in the full catalog code block created in Phase 5/6. No further edit if the file was created from that block.

### Success Criteria:

#### Automated Verification:
- [x] PlanMergeInto declared + defined: `grep -c 'PlanMergeInto' src/include/storage/calendar_catalog.hpp` returns 1 and `grep -c 'CalendarCatalog::PlanMergeInto' src/storage/calendar_catalog.cpp` returns 1
- [x] Reuses the three DML ops: `grep -cE 'CalendarInsert|CalendarUpdate|CalendarDelete' src/storage/calendar_merge.cpp` >= 3
- [x] Composes core PhysicalMergeInto: `grep -c 'planner.Make<PhysicalMergeInto>' src/storage/calendar_catalog.cpp` returns 1
- [x] No concrete-type coupling: `grep -c 'Cast<PhysicalUpdate>' src/storage/calendar_merge.cpp` returns 0

#### Manual Verification:
- [ ] `MERGE INTO cal.x AS t USING src AS s ON t.id=s.id WHEN MATCHED THEN UPDATE ... WHEN NOT MATCHED THEN INSERT ...` performs per-row PUT/POST
- [ ] `WHEN MATCHED THEN DELETE` removes events
- [ ] MERGE serialized (parallel=false); one HTTP call per affected row

---

## Phase 10: Tests + docs

### Overview
Replace the bootstrap-renamed scalar-fn test with a credential-free smoke test, add the credential-gated `require-env` live test, write the README, and apply the primary-calendar naming cascade. Owns the project-baseline build/test success criteria.

### Changes Required:

#### 1. Credential-free smoke test (MODIFY)
**File**: `test/sql/google_calendar.test`

```text
# name: test/sql/google_calendar.test
# description: Credential-free smoke test for the google_calendar extension
# group: [sql]

require google_calendar

# The google_calendar secret type is registered (access_token provider works offline)
statement ok
CREATE SECRET gcal_smoke (TYPE google_calendar, PROVIDER access_token, token 'dummy-token');

# ATTACH referencing a missing secret fails fast with a clear message (no network call)
statement error
ATTACH 'primary' AS cal (TYPE google_calendar, SECRET does_not_exist);
----
not found
```

#### 2. Credential-gated live test (NEW)
**File**: `test/sql/live.test`

```text
# name: test/sql/live.test
# description: Credential-gated live Google Calendar integration test (skipped unless GOOGLE_CALENDAR_ACCESS_TOKEN is set)
# group: [sql]

require google_calendar

require-env GOOGLE_CALENDAR_ACCESS_TOKEN

statement ok
CREATE SECRET gcal_live (TYPE google_calendar, PROVIDER access_token, token '${GOOGLE_CALENDAR_ACCESS_TOKEN}');

statement ok
ATTACH 'primary' AS cal (TYPE google_calendar, SECRET gcal_live);

# Bounded read (an explicit time bound on start is required)
query I
SELECT count(*) >= 0 FROM cal.primary
WHERE start >= TIMESTAMPTZ '2020-01-01 00:00+00' AND start < TIMESTAMPTZ '2020-01-08 00:00+00';
----
true

# Unbounded read is rejected
statement error
SELECT * FROM cal.primary;
----
explicit time bound
```

#### 3. README (MODIFY)
**File**: `README.md`

```markdown
# duckdb-google-calendar

A DuckDB extension that exposes Google Calendar as an attachable, read/write catalog
(`ATTACH ... (TYPE google_calendar)`), with one `events` table per calendar.

## Build

    make          # builds the extension + a duckdb shell with it preloaded
    make test     # runs test/sql/*.test (live tests need credentials; see below)

## Authenticate

Create a `google_calendar` secret (one of three providers):

    -- Service account (recommended for automation)
    CREATE SECRET cal (TYPE google_calendar, PROVIDER key_file, filepath '/path/sa.json');

    -- Pre-obtained OAuth access token
    CREATE SECRET cal (TYPE google_calendar, PROVIDER access_token, token '...');

    -- Interactive OAuth (requires a registered app):
    --   export GOOGLE_CALENDAR_OAUTH_CLIENT_ID=...   # optional: GOOGLE_CALENDAR_OAUTH_REDIRECT_URI
    CREATE SECRET cal (TYPE google_calendar, PROVIDER oauth);

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
```

#### 4. Primary-calendar naming cascade (MODIFY)
**File**: `src/storage/calendar_catalog.cpp`
**Changes**: `LoadCatalog` names the `primary==true` calendar exactly `"primary"` (so `cal.primary` matches the Desired End State), else `SlugifyName`. **Already present** in the full catalog code block (the design's end-state "Slice 6 state" snapshot rendered `cal.primary ? "primary" : SlugifyName(...)`). No further edit if the file was created from that block; otherwise apply the ternary in `LoadCatalog`.

### Success Criteria:

#### Automated Verification:
- [x] Project builds: `make` (or `make debug`) completes without error
- [x] Tests pass: `make test` (live tests skip without `GOOGLE_CALENDAR_ACCESS_TOKEN`)
- [x] No stale scalar-fn test: `grep -c "google_calendar('" test/sql/google_calendar.test` returns 0
- [x] Live test credential-gated: `grep -c 'require-env GOOGLE_CALENDAR_ACCESS_TOKEN' test/sql/live.test` returns 1
- [x] Primary calendar named `primary`: `grep -c 'cal.primary ? "primary"' src/storage/calendar_catalog.cpp` returns 1
- [x] No `waddle` anywhere: `grep -ril waddle . --exclude-dir=duckdb --exclude-dir=reference --exclude-dir=extension-ci-tools --exclude-dir=.git --exclude-dir=.rpiv --exclude-dir=.pi --exclude-dir=docs | wc -l` returns 0

#### Manual Verification:
- [ ] `make test` green in CI without credentials (smoke passes; live skipped)
- [ ] With `GOOGLE_CALENDAR_ACCESS_TOKEN` set, `live.test` attaches and reads a bounded window from `cal.primary`

---

## Testing Strategy

### Automated:
- Phase 10 gates the full build (`make` / `make debug`) and `make test` (credential-free smoke + skipped live).
- Intermediate phases use structural Automated Verification (grep / compile-unit presence) — they do not build standalone because `EXTENSION_SOURCES` (Phase 1) references not-yet-created files. This is expected for a foundation-first decomposition.

### Manual Testing Steps (reference — per-phase Manual Verification is load-bearing):
1. **VARCHAR rowid** — `EXPLAIN DELETE FROM cal.x WHERE id='…'` shows the rowid column as VARCHAR + the EXTENSION delete operator; the sink reads `FlatVector::GetData<string_t>`.
2. **Required time bound** — `SELECT * FROM cal.x` (no `start` predicate) raises a binder/runtime error before any HTTP call (assert no recorded request).
3. **Pushdown residual** — `start >= A AND start < B` (a) sets `timeMin`/`timeMax` on the recorded request URL and (b) still filters exactly (residual `LogicalFilter` retained). Verify via recorded request URL + result correctness against an over-wide fixture.
4. **Retry** — fixture `429,429,200` → `GetRecordedRequests().size() == 3` and query succeeds; non-retryable `403 quotaExceeded` fails fast (1 request).
5. **Pagination** — fixture with `nextPageToken` across 2 pages → 2 recorded list requests, rows concatenated.
6. **Name collisions** — two calendars with identical `summary` produce two distinct, stable table names; the primary calendar is `cal.primary`.
7. **Rename completeness** — after `bootstrap-template.py`: `grep -ri waddle . --exclude-dir=duckdb --exclude-dir=reference --exclude-dir=extension-ci-tools` returns nothing; CI `extension_name` + `extension_config.cmake` target both read `google_calendar`.

> Most of the above are **manual or live-only in v1** (mock suite deferred to v2). They re-enable as automated checks when the v2 mock seam lands (per-connection setting + request-matching mock).

## Performance Considerations

- DML serialized (`ParallelSink()==false`) for predictable rate-limit behavior; one HTTP round-trip per row (UPDATE = two: GET+PUT). Acceptable for automation-scale volumes; bulk paths are out of scope for v1.
- Scan over-fetches (API window ⊇ exact predicate) then DuckDB applies the residual — extra rows fetched are bounded by the asymmetry of `timeMin`/`timeMax` (event duration), not unbounded.
- Pagination streams pages into chunked emission via `GlobalTableFunctionState`; no full materialization beyond a page buffer.
- Retry backoff is bounded (max 5 attempts); sleep zeroable under tests so the suite stays fast.

## Migration Notes

Not applicable — no persisted schema or on-disk format (API-backed catalog, `IsDuckCatalog()==false`, no `SingleFileStorageManager`). No rollback/backwards-compat surface.

## Developer Context

**Step 5 triage (2026-06-10):** 4 reviewer findings (0 blockers, 2 concerns, 2 suggestions), all applied.
- Both `grep -ril waddle` gates (Phase 1 + Phase 10) gained `--exclude-dir=.rpiv --exclude-dir=.pi --exclude-dir=docs` so artifact text no longer trips the rename gate.
- `calendar_auth.cpp` include changed `<json.hpp>` → `"json.hpp"` (plan-local; design has the verbatim `<...>` form — minor design follow-up noted in the review table).
- Submodule-leak note resolved more strongly than recommended: per developer direction, `reference/duckdb_gsheets` is deleted entirely in Phase 1 (right after `json.hpp` is vendored — its only build-time use), with a `test ! -e reference/duckdb_gsheets` Success Criterion. "What We're NOT Doing" updated accordingly. Implementers consult the design's `## Pattern References` paths for guidance but transcribe code from this plan's fences (the live reference files are removed).

## Plan Review (Step 4)

_Independent post-finalization review by artifact-code-reviewer and artifact-coverage-reviewer subagents. Findings triaged at Step 5._

| source   | plan-loc                       | codebase-loc                                                              | severity   | dimension             | finding                                                                                                                                                                                              | recommendation                                                                                                                                       | resolution         |
| -------- | ------------------------------ | ------------------------------------------------------------------------- | ---------- | --------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------ |
| code     | Phase 1 (Success Criteria)     | research artifact contains "waddle" verbatim                              | concern    | actionability         | The "no `waddle` literals remain" gate `grep -ril waddle . --exclude-dir=duckdb --exclude-dir=reference --exclude-dir=extension-ci-tools --exclude-dir=.git` does not exclude `.rpiv`/`.pi`, and the research/design/plan artifacts under `.rpiv/` contain "waddle", so the check returns non-zero even after a clean rename | Add `--exclude-dir=.rpiv --exclude-dir=.pi --exclude-dir=docs` to the grep in this criterion | applied: added `.rpiv`/`.pi`/`docs` excludes to the Phase 1 grep gate |
| code     | Phase 10 (Success Criteria)    | this plan artifact contains "waddle"                                      | concern    | actionability         | The terminal "No `waddle` anywhere" gate reuses the same `grep -ril waddle .` without excluding `.rpiv`/`.pi`, so the plan artifact itself trips the check and the final phase falsely fails        | Add `--exclude-dir=.rpiv --exclude-dir=.pi --exclude-dir=docs` to this grep so the gate reflects only source-tree state | applied: added `.rpiv`/`.pi`/`docs` excludes to the Phase 10 grep gate |
| code     | Phase 3 §5 (calendar_auth.cpp) | sibling ported TUs use `"json.hpp"`                                       | suggestion | codebase-fit          | `calendar_auth.cpp` uses `#include <json.hpp>` (angle brackets) whereas every other ported TU (`client_factory.cpp`, `service_account_auth.cpp`, `events.cpp`, `response.hpp`) uses `#include "json.hpp"` | Change to `#include "json.hpp"` to match the convention of the sibling ported files (note: transcribed verbatim from design — design-root) | applied (plan-local; design follow-up: `.rpiv/artifacts/designs/2026-06-09_13-32-55_google-calendar-catalog-extension.md` `calendar_auth.cpp`): changed `<json.hpp>` → `"json.hpp"` in the Phase 3 fence |
| coverage | ## Verification Notes §17      | <n/a>                                                                     | suggestion | verification-coverage | Design note "confirm `reference/duckdb_gsheets` stays dev-only and does not leak into the distributed extension" — a must-not-leak intent, but no phase Success Criteria covers it and no packaging guard exists; entry is self-disclaimed "out of design scope; flagged for release checklist" | Add a Phase 10 Automated Verification bullet asserting the distributed loadable extension excludes the submodule, or explicitly record it on the named release checklist artifact | applied (stronger than recommended): per developer, the `reference/duckdb_gsheets` submodule is deleted entirely in Phase 1 (after vendoring `json.hpp`) with a `test ! -e reference/duckdb_gsheets` Success Criterion — the leak intent is closed because the source no longer exists |

## References

- Design: `.rpiv/artifacts/designs/2026-06-09_13-32-55_google-calendar-catalog-extension.md`
- Research: `.rpiv/artifacts/research/2026-06-09_11-25-22_google-calendar-catalog-extension.md`
- FRD: `.rpiv/artifacts/discover/2026-06-09_10-56-24_google-calendar-catalog-extension.md`
- Port source: `reference/duckdb_gsheets/` (submodule, dev-only)
- Pinned DuckDB headers: `duckdb/src/include/duckdb/{storage,catalog,transaction,function,planner}`
