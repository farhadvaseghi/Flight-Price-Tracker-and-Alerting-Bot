cmake_minimum_required(VERSION 3.20)
project(flight_tracker LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Everything static -> a single .exe with no DLLs to ship next to it.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# On MSVC, make our code and all dependencies agree on the CRT flavour.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_compile_options(/utf-8)
endif()

include(FetchContent)

# Timestamp extracted archives at extraction time, so a re-download actually
# retriggers dependent steps. The policy only exists on CMake >= 3.24; the
# DOWNLOAD_EXTRACT_TIMESTAMP option that replaces it does NOT exist on 3.22,
# where it would silently be parsed as part of the URL list.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# ---------------------------------------------------------------------------
# nlohmann/json  (header-only)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# ---------------------------------------------------------------------------
# cpr  (C++ Requests -> wraps libcurl, which it fetches itself)
# ---------------------------------------------------------------------------
set(CPR_ENABLE_SSL      ON  CACHE BOOL "" FORCE)
set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)  # let cpr build libcurl for us
set(CPR_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(BUILD_CPR_TESTS     OFF CACHE BOOL "" FORCE)

if(WIN32)
    # Use Windows' own TLS stack (Schannel) so we don't need OpenSSL installed.
    set(CURL_USE_SCHANNEL ON  CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL  OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL   OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG        1.10.5
    GIT_SHALLOW    TRUE
)

# ---------------------------------------------------------------------------
# SQLite3  (official amalgamation: two files, no build system of its own)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip
)

FetchContent_MakeAvailable(nlohmann_json cpr sqlite3)

# The amalgamation ships no CMakeLists.txt, so we wrap it in a target ourselves.
add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_COLUMN_METADATA
    SQLITE_OMIT_LOAD_EXTENSION
)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Our executable
# ---------------------------------------------------------------------------
add_executable(flight_tracker
    src/main.cpp
    src/database.cpp
    src/api_client.cpp
    src/notifier.cpp
)

target_link_libraries(flight_tracker PRIVATE
    cpr::cpr
    nlohmann_json::nlohmann_json
    sqlite3
)

if(WIN32)
    # curl drags in windows.h, which defines min and max as macros and breaks
    # any std::min / std::max call that follows. MSVC reports it as
    # "C2589: illegal token on right side of '::'".
    target_compile_definitions(flight_tracker PRIVATE NOMINMAX)
endif()

# Drop the binary in build/bin regardless of generator/config.
set_target_properties(flight_tracker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
)
