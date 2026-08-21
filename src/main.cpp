// Flight Price Tracker - sweeps German airports for cheap return trips to
// Europe and Turkey, keeps the lowest price ever seen per city pair in SQLite,
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
    api::Credentials         amadeus;                  // secret
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
};

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

// Busiest German airports. More origins means more coverage but also more API
// calls per cycle, which is the free tier's real constraint.
std::vector<std::string> default_origins() {
    return {"FRA", "MUC", "BER", "DUS", "HAM", "CGN", "STR", "HAJ"};
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
    config.amadeus.client_id     = env_or("AMADEUS_CLIENT_ID", config.amadeus.client_id);
    config.amadeus.client_secret = env_or("AMADEUS_SECRET", config.amadeus.client_secret);
    config.amadeus.host          = env_or("AMADEUS_HOST", config.amadeus.host);

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--once")         config.run_once = true;
        else if (arg == "--dry-run") config.dry_run  = true;
        else if (arg == "--probe")   config.probe    = true;
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
                "  --cap=EUR          only alert below this price (default 100)\n"
                "  --origins=A,B,C    override the German airports to sweep\n"
                "  --interval=N       seconds between sweeps (default 21600)\n\n"
                "environment:\n"
                "  TELEGRAM_BOT_TOKEN    bot token from @BotFather\n"
                "  TELEGRAM_CHAT_ID      channel (@name) or numeric chat id\n"
                "  AMADEUS_CLIENT_ID     Amadeus Self-Service key\n"
                "  AMADEUS_SECRET        Amadeus Self-Service secret\n"
                "  FLIGHT_TRACKER_DB     database file (default flights.db)\n\n"
                "Without Amadeus credentials the tracker runs against a built-in\n"
                "mock payload, so the pipeline can be exercised offline.\n";
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

// "YYYY-MM-DD" for today plus `offset_days`, in UTC.
std::string date_offset(int offset_days) {
    const std::time_t when = std::time(nullptr) +
                             static_cast<std::time_t>(offset_days) * 86400;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &when);
#else
    gmtime_r(&when, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d");
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

api::SearchParams params_for(const Config& config, const std::string& origin) {
    api::SearchParams params;
    params.origin = origin;
    // Tomorrow onward: today's departures are not actionable.
    params.departure_window = date_offset(1) + "," + date_offset(config.search_days);
    params.min_nights = config.min_nights;
    params.max_nights = config.max_nights;
    // Ask the API for the cap too, so it returns fewer, more relevant rows.
    params.max_price  = config.price_cap;
    return params;
}

CycleStats run_cycle(const Config& config, db::Database& store) {
    CycleStats stats;
    const bool live = config.amadeus.present();

    for (const std::string& origin : config.origins) {
        if (g_shutdown_requested.load(std::memory_order_relaxed)) break;

        const std::string payload =
            live ? api::fetch_flight_data(config.amadeus, params_for(config, origin))
                 : api::mock_flight_data();
        ++stats.calls;

        const api::ParseResult parsed = api::parse_flight_offers(payload);
        stats.parsed   += static_cast<int>(parsed.offers.size());
        stats.filtered += parsed.filtered;
        stats.skipped  += static_cast<int>(parsed.problems.size());

        if (parsed.offers.empty() && !parsed.problems.empty()) {
            log() << "  " << origin << ": " << parsed.problems.front() << '\n';
        }

        for (const FlightOffer& offer : parsed.offers) {
            db::PriceUpdate update;
            try {
                update = store.record_price(offer);
                ++stats.stored;
            } catch (const db::Error& e) {
                // One bad row must not abort the sweep; the next cycle retries.
                log() << "  db error on " << offer.origin << "-" << offer.destination
                      << ": " << e.what() << '\n';
                continue;
            }

            if (offer.price <= config.price_cap) ++stats.below_cap;

            // Two conditions, deliberately. A record low above the cap is real
            // but not worth waking you for; a cheap price that is not a record
            // has already been alerted on.
            if (!update.new_low || offer.price > config.price_cap) continue;

            const geo::Place to = geo::lookup(offer.destination);
            log() << "  ALERT " << offer.origin << "->" << offer.destination
                  << " (" << to.city << ")  "
                  << money(update.previous_lowest, offer.currency) << " -> "
                  << money(update.current_price, offer.currency)
                  << "  " << offer.departure_date;
            if (!offer.return_date.empty()) {
                std::cout << " +" << offer.nights() << "n";
            }
            std::cout << '\n';

            if (config.dry_run) {
                ++stats.alerts;
                continue;
            }

            const notify::SendResult sent =
                notify::send_price_drop_alert(config.alerts, offer, update);

            if (sent.ok) {
                ++stats.alerts;
            } else {
                // The new low is already committed, so we will not re-alert for
                // it. A missed notification beats a duplicate storm, and the
                // next genuine drop still fires.
                ++stats.failed_alerts;
                log() << "  alert delivery failed (" << sent.status_code << "): "
                      << sent.error << '\n';
            }
        }
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

// One live call, printed raw. The fastest way to find out whether Amadeus'
// free test tier actually serves Flight Inspiration Search for a given origin,
// before trusting anything built on top of it.
int run_probe(const Config& config) {
    if (!config.amadeus.present()) {
        std::cerr << "--probe needs AMADEUS_CLIENT_ID and AMADEUS_SECRET\n";
        return 1;
    }

    const std::string origin = config.origins.front();
    const api::SearchParams params = params_for(config, origin);

    log() << "probing " << config.amadeus.host << " for origin " << origin << '\n';
    log() << "  window " << params.departure_window
          << ", " << params.min_nights << '-' << params.max_nights << " nights"
          << ", cap " << params.max_price << '\n';

    const std::string token = api::access_token(config.amadeus);
    if (token.empty()) {
        std::cerr << "token request failed: check the credentials and the host\n";
        return 1;
    }
    log() << "  token acquired (" << token.size() << " chars)\n";

    const std::string body = api::fetch_flight_data(config.amadeus, params);
    if (body.empty()) {
        std::cerr << "search returned nothing: this origin may be unsupported "
                     "on the free test tier\n";
        return 1;
    }

    std::cout << "\n--- raw response ---\n" << body << "\n--- end ---\n\n";

    const api::ParseResult parsed = api::parse_flight_offers(body);
    log() << "parsed " << parsed.offers.size() << " in-region offers, "
          << parsed.filtered << " filtered out, "
          << parsed.problems.size() << " rejected\n";
    for (const FlightOffer& offer : parsed.offers) {
        log() << "  " << offer.origin << "->" << offer.destination
              << "  " << money(offer.price, offer.currency)
              << "  " << offer.departure_date << " +" << offer.nights() << "n\n";
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

    if (config.probe) return run_probe(config);

    const bool live = config.amadeus.present();

    log() << "flight tracker starting\n";
    log() << "  source   : " << (live ? "Amadeus (" + config.amadeus.host + ")"
                                      : "built-in mock (no credentials)") << '\n';
    log() << "  origins  : " << config.origins.size() << " German airports\n";
    log() << "  window   : next " << config.search_days << " days, "
          << config.min_nights << '-' << config.max_nights << " nights\n";
    log() << "  cap      : " << money(config.price_cap, "EUR") << '\n';
    log() << "  region   : " << geo::size() << " airports in Europe and Turkey\n";
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
                      << stats.filtered << " out of region, "
                      << stats.skipped << " rejected, "
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
