# Flight Price Tracker & Alerting Bot

[![build](https://github.com/farhadvaseghi/Flight-Price-Tracker-and-Alerting-Bot/actions/workflows/build.yml/badge.svg)](https://github.com/farhadvaseghi/Flight-Price-Tracker-and-Alerting-Bot/actions/workflows/build.yml)

Sweeps German airports for cheap return trips around Europe, remembers the
lowest price ever seen for each city pair in SQLite, and pings a Telegram
channel when a new low lands under your price cap and is meaningfully cheaper
than the last price you were told about.

Fares come from **Ryanair's public fare finder** -- the JSON endpoint their own
website calls. It needs no API key, no account and no signup, which is what
makes the whole system free to run. One request returns 20 fares from one
origin, filtered by date window and trip length server-side.

**Two things to know about that choice.** The endpoint is undocumented, so it
can change without notice -- `--probe` exists to tell you when it has. And it
only knows Ryanair's own network, which covers Europe well and Turkey not at
all, so Turkey is deliberately out of scope.

> Earlier versions used Amadeus, then Travelpayouts. Amadeus decommissioned its
> free Self-Service tier on 17 July 2026; Travelpayouts works but requires an
> affiliate signup. Each swap touched only `api_client.*`.

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

Run fully offline against a built-in payload:

```
flight_tracker --once --dry-run --mock
```

Check the live endpoint any time -- no credential needed:

```
flight_tracker --probe
```

| Flag              | Effect                                          |
|-------------------|-------------------------------------------------|
| `--once`          | run a single sweep and exit                     |
| `--dry-run`       | print alerts instead of posting them            |
| `--probe`         | one live API call, dumped raw                   |
| `--test-alert`    | send one sample alert to Telegram and exit      |
| `--cap=EUR`       | only alert below this price (default 100)       |
| `--min-drop=N`    | only alert on a drop of at least N% (default 10)|
| `--origins=A,B,C` | override the German airports to sweep           |
| `--interval=N`    | seconds between sweeps (default 21600, min 10)  |

Configuration is layered, later sources winning:
**defaults → `config.json` → environment → CLI flags.**

| Setting        | `config.json`               | Environment            |
|----------------|-----------------------------|------------------------|
| Bot token      | — (secret, env only)         | `TELEGRAM_BOT_TOKEN`   |
| Chat id        | `telegram_chat_id`          | `TELEGRAM_CHAT_ID`     |
| Database       | `database_path`             | `FLIGHT_TRACKER_DB`    |
| Origins        | `origins`                   | —                      |
| Price cap      | `price_cap`                 | —                      |
| Drop threshold | `min_drop_percent`          | —                      |
| Date window    | `search_days`               | —                      |
| Trip length    | `min_nights` / `max_nights` | —                      |
| Interval       | `interval_seconds`          | —                      |

Copy `config.example.json` to `config.json` to get started. **`config.json` is
gitignored.** The three secrets are deliberately environment-only and cannot be
set in the file at all: anyone holding the bot token can post as your bot.

## Architecture

```
fetch_flight_data()      api_client.cpp   Ryanair roundTripFares, paged
        |
parse_flight_offers()    api_client.cpp   -> vector<FlightOffer>, non-EU/TR dropped
        |
geo::in_region()         geo.cpp          country allowlist, no API calls
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
| `geo.*`         | European country allowlist for filtering destinations  |
| `api_client.*`  | fetching, paging, defensive JSON parsing               |
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

### Why the drop threshold has its own baseline

An alert needs three things: a new record low, a price under the cap, and a drop
of at least `min_drop_percent`. That last one is measured against
`alert_baseline` -- the price you were last told about -- and **not** against
`lowest_price`.

The distinction matters. `lowest_price` is overwritten by every new low, whether
or not it was worth a message. Measuring the threshold against it would compare
each drop only with the drop before it, so a fare sliding 3% per sweep would
clear 10% on no single step and stay silent the whole way down while losing a
third of its price. Pinning the baseline until an alert actually fires makes
those small steps accumulate, so the threshold means *"cheaper than when you
last heard from me"* -- which is what someone setting one actually wants.

The baseline moves only on a delivered message. A failed send leaves it where it
was, so the drop stays pending rather than being silently swallowed.

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

No flight-API credential appears here because the fare endpoint does not have
one.

Confirm the Telegram side works before waiting on a real price drop:

```
TELEGRAM_BOT_TOKEN=... TELEGRAM_CHAT_ID=@your_channel ./flight_tracker --test-alert
```

It sends one sample alert and explains the common failures: 401 is a bad token,
400 is usually a chat id missing its `@` prefix, 403 means the bot may not post
in that channel.

To alert a channel: create it in Telegram, add your bot as an **administrator**
with permission to post, and use `@channelname` as the chat id. For a private
chat instead, message the bot once and read the numeric id from
`https://api.telegram.org/bot<TOKEN>/getUpdates`.

The fare API needs no credential, so the sweep always runs and builds price
history. Without the Telegram secrets it records prices but warns that it cannot
alert.

**Request arithmetic.** A page is 20 fares, capped at 5 pages per origin, so a
sweep of 10 origins is at most 200 requests, paced 250ms apart. Four sweeps a
day is well-mannered for an endpoint that owes us nothing. Fares do not move
hourly, so polling harder buys nothing.

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
- [x] Ryanair fare-finder integration, keyless and paged
- [x] European region filter by country
- [x] Scheduled sweeps on GitHub Actions with committed price history
- [x] Live path verified: 18 real offers parsed from CGN, 36 city pairs stored
