# Flight Price Tracker & Alerting Bot

[![build](https://github.com/farhadvaseghi/Flight-Price-Tracker-and-Alerting-Bot/actions/workflows/build.yml/badge.svg)](https://github.com/farhadvaseghi/Flight-Price-Tracker-and-Alerting-Bot/actions/workflows/build.yml)

A small C++17 service that polls a flight-price REST API, stores the lowest price
ever seen per route in SQLite, and fires a Discord webhook whenever a new low
appears.

## Stack

| Concern      | Choice                                            |
|--------------|---------------------------------------------------|
| Language     | C++17                                             |
| Build        | CMake 3.20+ (dependencies via `FetchContent`)     |
| HTTP         | [cpr](https://github.com/libcpr/cpr) (libcurl)    |
| JSON         | [nlohmann/json](https://github.com/nlohmann/json) |
| Persistence  | SQLite3 (official amalgamation)                   |

No dependency needs to be installed by hand — CMake downloads and builds all of
them, including libcurl. On Windows, TLS is provided by Schannel, so there is no
OpenSSL install and no `cacert.pem` to ship.

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

With MinGW rather than MSVC, add `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`.
With Visual Studio installed, `-G "Visual Studio 17 2022" -A x64` works too.

The binary lands in `build/bin/flight_tracker.exe`. The first configure pulls
cpr, libcurl, zlib, nlohmann/json and the SQLite amalgamation, so it takes a few
minutes; later builds are incremental.

## Run

```
flight_tracker --dry-run --once
```

| Flag             | Effect                                       |
|------------------|----------------------------------------------|
| `--once`         | run a single check and exit                  |
| `--dry-run`      | print alerts instead of posting them         |
| `--interval=N`   | seconds between checks (default 900, min 10) |

Configuration is layered, later sources winning:
**defaults → `config.json` → environment → CLI flags.**

| Setting     | `config.json`        | Environment             |
|-------------|----------------------|-------------------------|
| Webhook URL | `webhook_url`        | `DISCORD_WEBHOOK_URL`   |
| Database    | `database_path`      | `FLIGHT_TRACKER_DB`     |
| Interval    | `interval_seconds`   | —                       |
| New routes  | `alert_on_new_route` | —                       |

Copy `config.example.json` to `config.json` to get started. **`config.json` is
gitignored** — the webhook URL is a credential, and anyone holding it can post
to your channel.

## Architecture

```
fetch_flight_data()      api_client.cpp   mock payload; swap for one cpr::Get
        |
parse_flight_offers()    api_client.cpp   -> vector<FlightOffer> + rejections
        |
record_price()           database.cpp     one transaction: is this a new low?
        |
send_price_drop_alert()  notifier.cpp     cpr::Post to the Discord webhook
        |
main loop                main.cpp         timer, signals, per-cycle recovery
```

| File            | Responsibility                                       |
|-----------------|------------------------------------------------------|
| `flight.h`      | `FlightOffer`, the type shared by every layer         |
| `api_client.*`  | fetching and defensive JSON parsing                   |
| `database.*`    | SQLite persistence, new-low detection                 |
| `notifier.*`    | Discord webhook delivery with retries                 |
| `main.cpp`      | configuration, polling loop, graceful shutdown        |

## Going live

`fetch_flight_data()` returns a hardcoded payload. Replace its body with a real
request — the shape is already written out in a comment there — and nothing else
changes, because the parser already handles empty and malformed bodies.

If your API's JSON differs from the mock's, `parse_flight_offers()` is the only
other place to touch.

## Testing

`tools/mock_webhook_server.py` stands in for Discord on `127.0.0.1:8099`,
returning whatever status code the path asks for:

```
python tools/mock_webhook_server.py
```

Then point the tracker at it:

```
DISCORD_WEBHOOK_URL=http://127.0.0.1:8099/status/204 ./flight_tracker --once
```

`/status/429` returns two rate-limit responses before succeeding, which
exercises the retry path.

## Continuous integration

`.github/workflows/build.yml` builds on every push and pull request, on
Windows with MSVC and on Linux with GCC, then runs `--once --dry-run` as a
smoke test and uploads the binary as an artifact. Fetched dependencies are
cached against the hash of `CMakeLists.txt`, so only a version bump pays the
full libcurl build again.

## Status

- [x] Step 1 — build system
- [x] Step 2 — SQLite persistence layer
- [x] Step 3 — JSON parsing of the (mocked) API response
- [x] Step 4 — Discord webhook alerting
- [x] Step 5 — main polling loop
