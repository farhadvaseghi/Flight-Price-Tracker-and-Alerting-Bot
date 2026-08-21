// Flight Price Tracker - polls a flight price API on a timer, keeps the
// lowest price ever seen per route in SQLite, and alerts a Discord channel
// whenever a new low appears.

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

#include <nlohmann/json.hpp>

#include "api_client.h"
#include "database.h"
#include "flight.h"
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
    std::string webhook_url;                  // secret; never hardcoded
    std::string database_path      = "flights.db";
    int         interval_seconds   = 900;     // 15 minutes
    bool        alert_on_new_route = false;   // a first sighting is not a drop
    bool        dry_run            = false;   // print alerts instead of posting
    bool        run_once           = false;   // one cycle, then exit
};

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

// Precedence: built-in defaults < config.json < environment < CLI flags.
// The environment beats the file so a container can override it without a
// rebuild; flags beat everything so a manual run can always force behaviour.
Config load_config(int argc, char** argv) {
    Config config;

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
            if (doc.contains("webhook_url") && doc["webhook_url"].is_string())
                config.webhook_url = doc["webhook_url"].get<std::string>();
            if (doc.contains("database_path") && doc["database_path"].is_string())
                config.database_path = doc["database_path"].get<std::string>();
            if (doc.contains("interval_seconds") && doc["interval_seconds"].is_number_integer())
                config.interval_seconds = doc["interval_seconds"].get<int>();
            if (doc.contains("alert_on_new_route") && doc["alert_on_new_route"].is_boolean())
                config.alert_on_new_route = doc["alert_on_new_route"].get<bool>();
        }
    }

    config.webhook_url   = env_or("DISCORD_WEBHOOK_URL", config.webhook_url);
    config.database_path = env_or("FLIGHT_TRACKER_DB", config.database_path);

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--once")            config.run_once = true;
        else if (arg == "--dry-run")    config.dry_run  = true;
        else if (arg.rfind("--interval=", 0) == 0) {
            try {
                config.interval_seconds = std::stoi(arg.substr(11));
            } catch (const std::exception&) {
                std::cerr << "warning: bad --interval, keeping "
                          << config.interval_seconds << "s\n";
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "usage: flight_tracker [--once] [--dry-run] [--interval=SECONDS]\n\n"
                "  --once            run a single check and exit\n"
                "  --dry-run         print alerts instead of posting them\n"
                "  --interval=N      seconds between checks (default 900)\n\n"
                "environment:\n"
                "  DISCORD_WEBHOOK_URL   webhook to post alerts to\n"
                "  FLIGHT_TRACKER_DB     database file (default flights.db)\n";
            std::exit(0);
        } else {
            std::cerr << "warning: ignoring unknown argument '" << arg << "'\n";
        }
    }

    // A one-second poll would get us rate limited by any real API.
    if (config.interval_seconds < 10) config.interval_seconds = 10;

    return config;
}

// ---------------------------------------------------------------------------
// Logging
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

std::ostream& log() {
    return std::cout << '[' << timestamp() << "] ";
}

std::string money(double amount, const std::string& currency) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << amount << ' ' << currency;
    return out.str();
}

// ---------------------------------------------------------------------------
// One polling cycle
// ---------------------------------------------------------------------------

struct CycleStats {
    int parsed  = 0;
    int skipped = 0;
    int stored  = 0;
    int alerts  = 0;
    int failed_alerts = 0;
};

CycleStats run_cycle(const Config& config, db::Database& store) {
    CycleStats stats;

    const api::ParseResult parsed = api::parse_flight_offers(api::fetch_flight_data());
    stats.parsed  = static_cast<int>(parsed.offers.size());
    stats.skipped = static_cast<int>(parsed.problems.size());

    for (const std::string& problem : parsed.problems) {
        log() << "  skipped: " << problem << '\n';
    }

    for (const FlightOffer& offer : parsed.offers) {
        db::PriceUpdate update;
        try {
            update = store.record_price(offer);
            ++stats.stored;
        } catch (const db::Error& e) {
            // One bad row must not abort the cycle; the next poll retries it.
            log() << "  db error on " << offer.origin << "->" << offer.destination
                  << ": " << e.what() << '\n';
            continue;
        }

        const bool worth_alerting =
            update.new_low || (update.first_sighting && config.alert_on_new_route);

        if (!worth_alerting) continue;

        log() << "  ALERT " << offer.origin << "->" << offer.destination
              << ' ' << offer.departure_date << "  "
              << money(update.previous_lowest, offer.currency) << " -> "
              << money(update.current_price, offer.currency) << '\n';

        if (config.dry_run) {
            log() << "  would post: "
                  << notify::build_price_drop_payload(offer, update) << '\n';
            ++stats.alerts;
            continue;
        }

        const notify::SendResult sent =
            notify::send_price_drop_alert(config.webhook_url, offer, update);

        if (sent.ok) {
            ++stats.alerts;
        } else {
            // The new low is already committed, so we will not re-alert for it.
            // That is the deliberate trade: a missed notification beats a
            // duplicate storm, and the next genuine drop still fires.
            ++stats.failed_alerts;
            log() << "  alert delivery failed (" << sent.status_code << "): "
                  << sent.error << '\n';
        }
    }

    return stats;
}

// Sleeps in short slices so Ctrl+C is answered within a second rather than
// after a 15 minute interval.
void interruptible_sleep(int seconds) {
    for (int elapsed = 0; elapsed < seconds; ++elapsed) {
        if (g_shutdown_requested.load(std::memory_order_relaxed)) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Redirected stdout is block buffered, so without this a service logging
    // to a file or to `docker logs` shows nothing until it exits or fills 4KB.
    std::cout << std::unitbuf;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    const Config config = load_config(argc, argv);

    log() << "flight tracker starting\n";
    log() << "  database : " << config.database_path << '\n';
    log() << "  interval : " << config.interval_seconds << "s\n";
    log() << "  mode     : "
          << (config.dry_run ? "dry run (no webhook posts)" : "live") << '\n';

    if (config.webhook_url.empty() && !config.dry_run) {
        log() << "  warning  : no DISCORD_WEBHOOK_URL set; alerts will fail\n";
    }

    try {
        db::Database store(config.database_path);
        store.initialize();

        int cycle = 0;
        while (!g_shutdown_requested.load(std::memory_order_relaxed)) {
            ++cycle;
            log() << "cycle " << cycle << " starting\n";

            try {
                const CycleStats stats = run_cycle(config, store);
                log() << "cycle " << cycle << " done: "
                      << stats.parsed << " parsed, "
                      << stats.skipped << " skipped, "
                      << stats.stored << " stored, "
                      << stats.alerts << " alerts";
                if (stats.failed_alerts > 0) {
                    std::cout << ", " << stats.failed_alerts << " undelivered";
                }
                std::cout << '\n';
            } catch (const std::exception& e) {
                // A single bad cycle (network blip, malformed batch) must not
                // end a process that is meant to run for weeks.
                log() << "cycle " << cycle << " failed: " << e.what() << '\n';
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
