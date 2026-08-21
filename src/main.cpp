// Flight Price Tracker - sweeps German airports for cheap return trips to
// Europe, keeps the lowest price ever seen per city pair in SQLite,
// and alerts a Telegram channel when a new low lands under the price cap.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api_client.h"
#include "database.h"
#include "flight.h"
#include "geo.h"
#include "notifier.h"

namespace {

// Set from the signal handler, so it must be a lock-free flag and nothing
// else: no allocation, no iostreams, no locking is legal in there.
std::atomic<bool> g_shutdown_requested{false};

extern "C" void handle_signal(int) {
    g_shutdown_requested.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    notify::Telegram         alerts;                   // bot token is secret
    api::Settings            flights;                  // no credential needed
    std::string              database_path = "flights.db";
    std::vector<std::string> origins;                  // German airports
    int         search_days      = 90;                 // how far ahead to look
    int         min_nights       = 2;
    int         max_nights       = 4;
    double      price_cap        = 100.0;              // only alert below this
    int         interval_seconds = 21600;              // 6 hours
    bool        dry_run          = false;
    bool        run_once         = false;
    bool        probe            = false;              // one live call, dump it
    bool        test_alert       = false;              // send one fake alert
};

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

// German airports Ryanair actually flies from, verified against the live
// endpoint. Frankfurt, Munich, Dusseldorf and Stuttgart are deliberately
// absent: Ryanair does not serve them, so querying them returns nothing and
// only costs requests.
std::vector<std::string> default_origins() {
    return {"BER", "CGN", "HHN", "NUE", "FMM", "HAM", "FKB", "FMO", "PAD", "BRE"};
}

// Precedence: built-in defaults < config.json < environment < CLI flags.
// The environment beats the file so CI can inject secrets without a commit;
// flags beat everything so a manual run can always force behaviour.
Config load_config(int argc, char** argv) {
    Config config;
    config.origins = default_origins();

    // is_open() rather than operator bool: on MinGW's libstdc++ the stream
    // still tests true after a failed open, so `if (file)` would send us into
    // the parse and warn about a config.json that does not exist.
    std::ifstream file("config.json");
    if (file.is_open()) {
        const nlohmann::json doc =
            nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded()) {
            std::cerr << "warning: config.json is not valid JSON, ignoring it\n";
        } else {
            // The chat id is not a secret; the bot token is, so it is env only.
            if (doc.contains("telegram_chat_id") && doc["telegram_chat_id"].is_string())
                config.alerts.chat_id = doc["telegram_chat_id"].get<std::string>();
            if (doc.contains("database_path") && doc["database_path"].is_string())
                config.database_path = doc["database_path"].get<std::string>();
            if (doc.contains("price_cap") && doc["price_cap"].is_number())
                config.price_cap = doc["price_cap"].get<double>();
            if (doc.contains("search_days") && doc["search_days"].is_number_integer())
                config.search_days = doc["search_days"].get<int>();
            if (doc.contains("min_nights") && doc["min_nights"].is_number_integer())
                config.min_nights = doc["min_nights"].get<int>();
            if (doc.contains("max_nights") && doc["max_nights"].is_number_integer())
                config.max_nights = doc["max_nights"].get<int>();
            if (doc.contains("interval_seconds") && doc["interval_seconds"].is_number_integer())
                config.interval_seconds = doc["interval_seconds"].get<int>();
            if (doc.contains("origins") && doc["origins"].is_array()) {
                std::vector<std::string> list;
                for (const auto& item : doc["origins"]) {
                    if (item.is_string()) list.push_back(item.get<std::string>());
                }
                if (!list.empty()) config.origins = list;
            }
        }
    }

    config.alerts.bot_token      = env_or("TELEGRAM_BOT_TOKEN", config.alerts.bot_token);
    config.alerts.chat_id        = env_or("TELEGRAM_CHAT_ID", config.alerts.chat_id);
    config.alerts.api_base       = env_or("TELEGRAM_API_BASE", config.alerts.api_base);
    config.database_path         = env_or("FLIGHT_TRACKER_DB", config.database_path);
    config.flights.host          = env_or("FLIGHT_API_HOST", config.flights.host);

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--once")         config.run_once = true;
        else if (arg == "--dry-run") config.dry_run  = true;
        else if (arg == "--probe")   config.probe    = true;
        else if (arg == "--mock")    config.flights.use_mock = true;
        else if (arg == "--test-alert") config.test_alert = true;
        else if (arg.rfind("--cap=", 0) == 0) {
            try { config.price_cap = std::stod(arg.substr(6)); }
            catch (const std::exception&) { std::cerr << "warning: bad --cap\n"; }
        } else if (arg.rfind("--interval=", 0) == 0) {
            try { config.interval_seconds = std::stoi(arg.substr(11)); }
            catch (const std::exception&) { std::cerr << "warning: bad --interval\n"; }
        } else if (arg.rfind("--origins=", 0) == 0) {
            std::vector<std::string> list;
            std::stringstream        parts(arg.substr(10));
            std::string              code;
            while (std::getline(parts, code, ',')) {
                if (!code.empty()) list.push_back(code);
            }
            if (!list.empty()) config.origins = list;
        } else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "usage: flight_tracker [options]\n\n"
                "  --once             run a single sweep and exit\n"
                "  --dry-run          print alerts instead of posting them\n"
                "  --probe            make one live API call and dump the raw response\n"
                "  --test-alert       send one sample alert to Telegram and exit\n"
                "  --mock             use the built-in payload, make no network calls\n"
                "  --cap=EUR          only alert below this price (default 100)\n"
                "  --origins=A,B,C    override the German airports to sweep\n"
                "  --interval=N       seconds between sweeps (default 21600)\n\n"
                "environment:\n"
                "  TELEGRAM_BOT_TOKEN    bot token from @BotFather\n"
                "  TELEGRAM_CHAT_ID      channel (@name) or numeric chat id\n"
                "  FLIGHT_TRACKER_DB     database file (default flights.db)\n\n"
                "Fares come from Ryanair's public fare finder, which needs no key\n"
                "and no account. --mock swaps in a built-in payload so the whole\n"
                "pipeline can be exercised offline.\n";
            std::exit(0);
        } else {
            std::cerr << "warning: ignoring unknown argument '" << arg << "'\n";
        }
    }

    if (config.interval_seconds < 10) config.interval_seconds = 10;
    return config;
}

// ---------------------------------------------------------------------------
// Logging and dates
// ---------------------------------------------------------------------------

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::ostream& log() { return std::cout << '[' << timestamp() << "] "; }

std::string money(double amount, const std::string& currency) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << amount << ' ' << currency;
    return out.str();
}


// ---------------------------------------------------------------------------
// One sweep across every origin
// ---------------------------------------------------------------------------

struct CycleStats {
    int calls    = 0;
    int parsed   = 0;
    int filtered = 0;
    int skipped  = 0;
    int stored   = 0;
    int alerts   = 0;
    int failed_alerts = 0;
    int below_cap     = 0;
};

// The endpoint constrains dates and trip length server-side, so these travel
// with the request rather than being applied to the response.
api::SearchParams params_for(const Config& config, const std::string& origin,
                             int offset) {
    api::SearchParams params;
    params.origin = origin;
    // Tomorrow onward: today's departures are not actionable.
    params.earliest_departure = date_offset_utc(1);
    params.latest_departure   = date_offset_utc(config.search_days);
    params.min_nights         = config.min_nights;
    params.max_nights         = config.max_nights;
    params.currency           = "EUR";
    params.offset             = offset;
    return params;
}

// A page holds 20 fares and the endpoint rejects any larger limit, so several
// requests are needed per origin. Five pages is 100 fares, which in practice
// covers every destination an airport serves -- the extra fares are repeats of
// the same city pair on different dates, and only the cheapest is kept anyway.
//
// This endpoint is undocumented and free, and owes us nothing, so the pager is
// capped and paced rather than run flat out.
constexpr int kMaxPagesPerOrigin   = 5;
constexpr int kPauseBetweenCallsMs = 250;

// Stores one offer and alerts if it is a new low worth reporting.
void handle_offer(const Config& config, db::Database& store,
                  const FlightOffer& offer, CycleStats& stats) {
    db::PriceUpdate update;
    try {
        update = store.record_price(offer);
        ++stats.stored;
    } catch (const db::Error& e) {
        // One bad row must not abort the sweep; the next cycle retries it.
        log() << "  db error on " << offer.origin << "-" << offer.destination
              << ": " << e.what() << '\n';
        return;
    }

    if (offer.price <= config.price_cap) ++stats.below_cap;

    // Two conditions, deliberately. A record low above the cap is real but not
    // worth waking you for; a cheap price that is not a record has already been
    // alerted on.
    if (!update.new_low || offer.price > config.price_cap) return;

    log() << "  ALERT " << offer.origin << "->" << offer.destination
          << " (" << offer.destination_city << ")  "
          << money(update.previous_lowest, offer.currency) << " -> "
          << money(update.current_price, offer.currency)
          << "  " << offer.departure_date;
    if (!offer.return_date.empty()) std::cout << " +" << offer.nights() << "n";
    std::cout << '\n';

    if (config.dry_run) {
        ++stats.alerts;
        return;
    }

    const notify::SendResult sent =
        notify::send_price_drop_alert(config.alerts, offer, update);

    if (sent.ok) {
        ++stats.alerts;
    } else {
        // The new low is already committed, so we will not re-alert for it. A
        // missed notification beats a duplicate storm, and the next genuine
        // drop still fires.
        ++stats.failed_alerts;
        log() << "  alert delivery failed (" << sent.status_code << "): "
              << sent.error << '\n';
    }
}

CycleStats run_cycle(const Config& config, db::Database& store) {
    CycleStats stats;

    for (const std::string& origin : config.origins) {
        if (g_shutdown_requested.load(std::memory_order_relaxed)) break;

        int found = 0;

        // One page holds 20 fares, so an origin serving 100 destinations needs
        // five requests. Stop as soon as the endpoint says there is no next
        // page, and never exceed the cap.
        for (int page = 0; page < kMaxPagesPerOrigin; ++page) {
            if (g_shutdown_requested.load(std::memory_order_relaxed)) break;

            if (stats.calls > 0 && !config.flights.use_mock) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kPauseBetweenCallsMs));
            }

            const std::string payload = api::fetch_flight_data(
                config.flights, params_for(config, origin, page * api::kPageSize));
            ++stats.calls;

            const api::ParseResult parsed = api::parse_flight_offers(payload);
            stats.parsed   += static_cast<int>(parsed.offers.size());
            stats.filtered += parsed.out_of_region;
            stats.skipped  += static_cast<int>(parsed.problems.size());
            found          += static_cast<int>(parsed.offers.size());

            // Only complain about the first page. A later page failing is
            // usually just the end of the list.
            if (page == 0 && parsed.offers.empty() && !parsed.problems.empty()) {
                log() << "  " << origin << ": " << parsed.problems.front() << '\n';
            }

            for (const FlightOffer& offer : parsed.offers) {
                handle_offer(config, store, offer, stats);
            }

            // The mock is a single page by construction.
            if (!parsed.has_more || config.flights.use_mock) break;
        }

        if (found == 0) {
            // Not an error: Ryanair may simply not fly from here, or not
            // within the requested window.
            log() << "  " << origin << ": no fares in range\n";
        }
    }

    // A destination the allowlist does not know is dropped silently otherwise,
    // so surface it once per sweep rather than never.
    const std::vector<std::string> unknown = geo::unknown_countries();
    if (!unknown.empty()) {
        std::string list;
        for (const std::string& country : unknown) {
            if (!list.empty()) list += ", ";
            list += country;
        }
        log() << "  outside region: " << list << '\n';
    }

    return stats;
}

// Sleeps in short slices so Ctrl+C is answered within a second rather than
// after a six hour interval.
void interruptible_sleep(int seconds) {
    for (int elapsed = 0; elapsed < seconds; ++elapsed) {
        if (g_shutdown_requested.load(std::memory_order_relaxed)) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Sends one fabricated alert, so the Telegram side can be confirmed working
// without waiting for a real price drop. Exercises the whole delivery path:
// token, chat id, the bot's permission to post, and HTML rendering.
int run_test_alert(const Config& config) {
    if (!config.alerts.configured()) {
        std::cerr << "--test-alert needs TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_ID\n";
        return 1;
    }

    // A realistic offer, not a placeholder: the point of this flag is to show
    // exactly what a real alert will look like. It predates the move to
    // Ryanair, and for a while it still described FRA->IST -- an airport
    // Ryanair does not serve, to a country now out of scope -- with no city
    // names and no booking link, because those fields did not exist when it
    // was written. That made the one command meant to prove the setup works
    // misrepresent the thing it was proving.
    FlightOffer offer;
    offer.origin              = "CGN";
    offer.destination         = "BCN";
    offer.origin_city         = "Cologne";
    offer.destination_city    = "Barcelona";
    offer.destination_country = "Spain";
    offer.departure_date      = date_offset_utc(30);
    offer.return_date         = date_offset_utc(33);
    offer.currency            = "EUR";
    offer.price               = 89.0;
    offer.booking_link        = api::booking_url(offer);

    db::PriceUpdate update;
    update.previous_lowest = 164.5;
    update.current_price   = offer.price;
    update.new_low         = true;

    log() << "sending a sample alert to " << config.alerts.chat_id << '\n';
    std::cout << '\n' << notify::build_price_drop_message(offer, update) << "\n\n";

    const notify::SendResult sent =
        notify::send_price_drop_alert(config.alerts, offer, update);

    if (sent.ok) {
        log() << "delivered. Check the channel -- setup is working.\n";
        return 0;
    }

    log() << "delivery FAILED (" << sent.status_code << "): " << sent.error << '\n';

    // Name the three common failures, because the API's own wording is opaque.
    if (sent.status_code == 401) {
        log() << "  401: the bot token is wrong or has been revoked.\n";
    } else if (sent.status_code == 400) {
        log() << "  400: usually a wrong chat id. A channel needs the @ prefix,\n";
        log() << "       e.g. @my_channel, and must already exist.\n";
    } else if (sent.status_code == 403) {
        log() << "  403: the bot may not post there. Add it to the channel as an\n";
        log() << "       administrator with permission to post messages.\n";
    }
    return 1;
}

// One live call, printed raw, plus what the parser makes of it. The endpoint is
// undocumented, so this is how you find out it has changed shape without
// waiting for a silent sweep that finds nothing.
int run_probe(const Config& config) {
    const std::string       origin = config.origins.front();
    const api::SearchParams params = params_for(config, origin, 0);

    log() << "probing " << config.flights.host << " for origin " << origin << '\n';
    log() << "  window " << params.earliest_departure << " to "
          << params.latest_departure << ", " << params.min_nights << '-'
          << params.max_nights << " nights\n";

    const std::string body = api::fetch_flight_data(config.flights, params);
    if (body.empty()) {
        std::cerr << "request failed or returned nothing.\n"
                     "Ryanair may not fly from this airport -- try BER, CGN or FMM.\n";
        return 1;
    }

    std::cout << "\n--- raw response ---\n" << body << "\n--- end ---\n\n";

    const api::ParseResult parsed = api::parse_flight_offers(body);

    log() << "parsed " << parsed.offers.size() << " usable offers, "
          << parsed.out_of_region << " outside the region, "
          << parsed.problems.size() << " malformed"
          << (parsed.has_more ? ", more pages available" : "") << '\n';

    for (const std::string& problem : parsed.problems) {
        log() << "  malformed: " << problem << '\n';
    }

    for (const FlightOffer& offer : parsed.offers) {
        log() << "  " << offer.origin << "->" << offer.destination
              << "  " << money(offer.price, offer.currency)
              << "  " << offer.departure_date << " +" << offer.nights() << "n"
              << "  (" << offer.destination_city << ", "
              << offer.destination_country << ")\n";
    }

    const std::vector<std::string> unknown = geo::unknown_countries();
    for (const std::string& country : unknown) {
        log() << "  country not in the allowlist: " << country << '\n';
    }

    // A response that parses to nothing is the interesting failure, so say what
    // to try rather than leaving an empty list.
    if (parsed.offers.empty()) {
        log() << "nothing usable. Widen the window with a larger search_days, or\n"
                 "relax min_nights/max_nights, then probe again.\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Redirected stdout is block buffered, so without this a service logging
    // to a file or to `docker logs` shows nothing until it exits or fills 4KB.
    std::cout << std::unitbuf;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    const Config config = load_config(argc, argv);

    if (config.test_alert) return run_test_alert(config);
    if (config.probe)      return run_probe(config);

    log() << "flight tracker starting\n";
    log() << "  source   : " << (config.flights.use_mock
                                     ? "built-in mock (no network)"
                                     : config.flights.host) << '\n';
    log() << "  origins  : " << config.origins.size() << " German airports\n";
    log() << "  window   : next " << config.search_days << " days, "
          << config.min_nights << '-' << config.max_nights << " nights\n";
    log() << "  cap      : " << money(config.price_cap, "EUR") << '\n';
    log() << "  region   : " << geo::size() << " countries in Europe\n";
    log() << "  database : " << config.database_path << '\n';
    log() << "  mode     : " << (config.dry_run ? "dry run (nothing is sent)" : "live") << '\n';
    log() << "  alerts   : "
          << (config.alerts.configured() ? "Telegram chat " + config.alerts.chat_id
                                         : "not configured") << '\n';

    if (!config.alerts.configured() && !config.dry_run) {
        log() << "  warning  : no TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID; alerts will fail\n";
    }

    try {
        db::Database store(config.database_path);
        store.initialize();
        log() << "  tracking : " << store.route_count() << " city pairs so far\n";

        int cycle = 0;
        while (!g_shutdown_requested.load(std::memory_order_relaxed)) {
            ++cycle;
            log() << "sweep " << cycle << " starting\n";

            try {
                const CycleStats stats = run_cycle(config, store);
                log() << "sweep " << cycle << " done: "
                      << stats.calls << " calls, "
                      << stats.parsed << " offers, "
                      << stats.filtered << " filtered, "
                      << stats.skipped << " malformed, "
                      << stats.below_cap << " under cap, "
                      << stats.alerts << " alerts";
                if (stats.failed_alerts > 0) {
                    std::cout << ", " << stats.failed_alerts << " undelivered";
                }
                std::cout << '\n';
                log() << "tracking " << store.route_count() << " city pairs\n";
            } catch (const std::exception& e) {
                // A single bad sweep (network blip, malformed batch) must not
                // end a process that is meant to run for weeks.
                log() << "sweep " << cycle << " failed: " << e.what() << '\n';
            }

            if (config.run_once) break;

            log() << "sleeping " << config.interval_seconds << "s\n";
            interruptible_sleep(config.interval_seconds);
        }

    } catch (const db::Error& e) {
        // Only a failure to open or create the database lands here, and there
        // is no recovering from that.
        log() << "fatal: " << e.what() << '\n';
        return 1;
    }

    log() << "shutting down cleanly\n";
    return 0;
}
