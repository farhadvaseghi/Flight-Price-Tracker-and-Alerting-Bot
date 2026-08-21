# Flight Price Tracker & Alerting Bot

A small C++17 service that polls a flight-price REST API, stores the lowest price
seen per route in SQLite, and fires a Discord webhook whenever a new low appears.

## Stack

| Concern      | Choice                                            |
|--------------|---------------------------------------------------|
| Language     | C++17                                             |
| Build        | CMake 3.20+ (dependencies via `FetchContent`)     |
| HTTP         | [cpr](https://github.com/libcpr/cpr) (libcurl)    |
| JSON         | [nlohmann/json](https://github.com/nlohmann/json) |
| Persistence  | SQLite3 (official amalgamation)                   |

No dependency needs to be installed by hand — CMake downloads and builds all of
them, including libcurl. On Windows TLS is provided by Schannel, so no OpenSSL
install is required.

## Build (Windows)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The binary lands in `build/bin/flight_tracker.exe`.

The first configure downloads cpr, libcurl, nlohmann/json and the SQLite
amalgamation, so it takes a few minutes. Later configures are cached.

## Status

- [x] Step 1 — build system
- [ ] Step 2 — SQLite persistence layer
- [ ] Step 3 — JSON parsing of the (mocked) API response
- [ ] Step 4 — Discord webhook alerting
- [ ] Step 5 — main polling loop
