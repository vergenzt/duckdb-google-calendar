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
    src/calendar/resources/calendars.cpp
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

# macOS keychain (refresh-token caching in src/calendar_auth.cpp)
if(APPLE)
  target_link_libraries(${EXTENSION_NAME} "-framework Security" "-framework CoreFoundation")
  target_link_libraries(${LOADABLE_EXTENSION_NAME} "-framework Security" "-framework CoreFoundation")
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
