# Flight Price Tracker & Alerting Bot

[![build](https://github.com/farhadvaseghi/Flight-Price-Tracker-and-Alerting-Bot/actions/workflows/build.yml/badge.svg)](https://github.com/farhadvaseghi/Flight-Price-Tracker-and-Alerting-Bot/actions/workflows/build.yml)

Sweeps German airports for cheap return trips to Europe and Turkey, remembers the
lowest price ever seen for each city pair in SQLite, and pings a Telegram
channel when a new low lands under your price cap.

Data comes from Amadeus' **Flight Inspiration Search**, which returns every
destination reachable from one origin in a single call -- so a sweep of eight
German airports costs eight API calls, not hundreds. Destinations outside Europe
and Turkey are filtered out locally against a bundled airport table, which costs
no quota at all.

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

No credentials? It falls back to a built-in mock payload, so the whole pipeline
runs offline:

```
flight_tracker --once --dry-run
```

With an Amadeus key, check the live path before trusting it:

```
flight_tracker --probe
```

| Flag              | Effect                                          |
|-------------------|-------------------------------------------------|
| `--once`          | run a single sweep and exit                     |
| `--dry-run`       | print alerts instead of posting them            |
| `--probe`         | one live API call, dumped raw                   |
| `--cap=EUR`       | only alert below this price (default 100)       |
| `--origins=A,B,C` | override the German airports to sweep           |
| `--interval=N`    | seconds between sweeps (default 21600, min 10)  |

Configuration is layered, later sources winning:
**defaults → `config.json` → environment → CLI flags.**

| Setting        | `config.json`               | Environment            |
|----------------|-----------------------------|------------------------|
| Bot token      | — (secret, env only)         | `TELEGRAM_BOT_TOKEN`   |
| Chat id        | `telegram_chat_id`          | `TELEGRAM_CHAT_ID`     |
| Amadeus key    | — (secret, env only)         | `AMADEUS_CLIENT_ID`    |
| Amadeus secret | — (secret, env only)         | `AMADEUS_SECRET`       |
| Database       | `database_path`             | `FLIGHT_TRACKER_DB`    |
| Origins        | `origins`                   | —                      |
| Price cap      | `price_cap`                 | —                      |
| Date window    | `search_days`               | —                      |
| Trip length    | `min_nights` / `max_nights` | —                      |
| Interval       | `interval_seconds`          | —                      |

Copy `config.example.json` to `config.json` to get started. **`config.json` is
gitignored.** The three secrets are deliberately environment-only and cannot be
set in the file at all: anyone holding the bot token can post as your bot.

## Architecture

```
fetch_flight_data()      api_client.cpp   Amadeus Inspiration Search, 1 call/origin
        |
parse_flight_offers()    api_client.cpp   -> vector<FlightOffer>, non-EU/TR dropped
        |
geo::in_region()         geo.cpp          bundled airport table, no API calls
        |
record_price()           database.cpp     one transaction: new low for this city pair?
        |
send_price_drop_alert()  notifier.cpp     cpr::Post to the Telegram Bot API
        |
main loop                main.cpp         sweep, timer, signals, per-cycle recovery
```

| File            | Responsibility                                        |
|-----------------|-------------------------------------------------------|
| `flight.h/.cpp` | `FlightOffer`, the type shared by every layer          |
| `geo.*`         | Europe/Turkey airport allowlist and place names        |
| `api_client.*`  | Amadeus OAuth, fetching, defensive JSON parsing        |
| `database.*`    | SQLite persistence, new-low detection per city pair    |
| `notifier.*`    | Telegram delivery, HTML escaping, retries              |
| `main.cpp`      | configuration, sweep loop, graceful shutdown           |

### Why the city pair is the key

`routes` is keyed on `(origin, destination)`, not on the departure date. The
question this bot answers is *"is Frankfurt to Istanbul cheaper than it has ever
been"* -- the dates are the answer, not part of the question. Keying on the date
too would create thousands of rows that each need their own price history before
they could ever alert.

The dates behind the record price are stored as metadata and move with it,
so an alert can show both the new trip and the one it beat.

## Testing

`tools/mock_telegram_server.py` stands in for the Telegram Bot API on
`127.0.0.1:8099` and prints every message it receives, so the alert text can be
checked without sending anything real:

```
python tools/mock_telegram_server.py
```

The token in the URL selects the behaviour, which is how the delivery paths are
exercised:

| Token      | Response                                    |
|------------|---------------------------------------------|
| anything   | `200 {"ok": true}` -- normal success        |
| `fail`     | `401` -- revoked token, must not retry      |
| `limit`    | `429` twice with `retry_after`, then success |
| `broken`   | `500` -- must retry with backoff            |

```
TELEGRAM_API_BASE=http://127.0.0.1:8099 TELEGRAM_BOT_TOKEN=test TELEGRAM_CHAT_ID=@test ./flight_tracker --once
```

`TELEGRAM_API_BASE` exists only for this. Leave it unset in production.

## Running it free, forever

`.github/workflows/track.yml` runs a sweep every 6 hours on GitHub Actions,
which is free with unlimited minutes on public repositories -- no server, no
card. `flights.db` is committed back to the repo after each sweep, so the price
history survives between runs and doubles as a git-diffable record.

Add three repository secrets (Settings -> Secrets and variables -> Actions):

| Secret               | Where to get it                                     |
|----------------------|-----------------------------------------------------|
| `TELEGRAM_BOT_TOKEN` | Message @BotFather, `/newbot`, copy the token       |
| `TELEGRAM_CHAT_ID`   | `@your_channel`, or a numeric id for a private chat |
| `AMADEUS_CLIENT_ID`  | developers.amadeus.com, free Self-Service app       |
| `AMADEUS_SECRET`     | same app                                            |

To alert a channel: create it in Telegram, add your bot as an **administrator**
with permission to post, and use `@channelname` as the chat id. For a private
chat instead, message the bot once and read the numeric id from
`https://api.telegram.org/bot<TOKEN>/getUpdates`.

Without `AMADEUS_CLIENT_ID` the scheduled job warns and exits rather than
writing mock prices into the real history.

**Quota arithmetic.** One API call covers one origin, so:

```
8 origins x every 6 hours x 30 days =   960 calls/month
8 origins x every hour    x 30 days = 5,760 calls/month
```

Fares do not move hourly, so the 6-hour default costs nothing in practice.
Check the current quota on your Amadeus dashboard before raising it.

Two honest limits: GitHub runs scheduled jobs late under load and occasionally
skips one, which is irrelevant at this cadence; and scheduled workflows are
disabled after 60 days of repository inactivity, which the price-history commits
should prevent.

## Continuous integration

`.github/workflows/build.yml` builds on every push and pull request, on
Windows with MSVC and on Linux with GCC, then runs `--once --dry-run` as a
smoke test and uploads the binary as an artifact. Fetched dependencies are
cached against the hash of `CMakeLists.txt`, so only a version bump pays the
full libcurl build again.

## Status

- [x] Build system, SQLite store, JSON parsing, Telegram alerting, sweep loop
- [x] CI on Windows/MSVC and Linux/GCC
- [x] Amadeus Flight Inspiration Search with OAuth token caching
- [x] Europe/Turkey region filter
- [x] Scheduled sweeps on GitHub Actions with committed price history
- [ ] Verify the live Amadeus path with `--probe` (needs a key)
