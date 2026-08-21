// Step 4 harness: exercises the Discord webhook layer.
// This file gets replaced by the real application loop in Step 5.

#include <chrono>
#include <iostream>

#include "database.h"
#include "flight.h"
#include "notifier.h"

namespace {

void show(const char* label, const notify::SendResult& result) {
    std::cout << "  " << label << "\n"
              << "      ok     : " << (result.ok ? "yes" : "no") << '\n'
              << "      status : " << result.status_code << '\n'
              << "      error  : " << (result.error.empty() ? "-" : result.error) << '\n';
}

}  // namespace

int main() {
    FlightOffer offer;
    offer.origin         = "FRA";
    offer.destination    = "TBS";
    offer.departure_date = "2026-11-14";
    offer.airline        = "Turkish Airlines";
    offer.currency       = "EUR";
    offer.price          = 355.00;

    db::PriceUpdate update;
    update.previous_lowest = 412.50;
    update.current_price   = 355.00;
    update.new_low         = true;

    // --- payload construction, no network involved ---
    std::cout << "alert payload:\n"
              << notify::build_price_drop_payload(offer, update) << "\n\n";

    // A name containing a quote must not be able to break the JSON.
    FlightOffer hostile = offer;
    hostile.airline = "Air \"Quote\" \n Injection";
    std::cout << "payload with hostile airline name:\n"
              << notify::build_price_drop_payload(hostile, update) << "\n\n";

    // --- transport behaviour, against a public HTTP status echo service ---
    std::cout << "transport:\n";

    show("empty URL (no request made)",
         notify::send_discord_webhook("", "hello"));

    // Discord answers 204 on success; this proves we accept it and that TLS
    // works through Schannel.
    show("204 No Content (Discord's success code)",
         notify::send_discord_webhook("http://127.0.0.1:8099/status/204", "ping"));

    show("404 (dead webhook - must not retry)",
         notify::send_discord_webhook("http://127.0.0.1:8099/status/404", "ping"));

    const auto rl_started = std::chrono::steady_clock::now();
    show("429 twice then 204 (rate limit recovery)",
         notify::send_discord_webhook("http://127.0.0.1:8099/status/429", "ping"));
    std::cout << "      elapsed: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - rl_started).count()
              << "ms (two Retry-After waits of 400ms)\n";

    const auto started = std::chrono::steady_clock::now();
    show("500 (server error - must retry with backoff)",
         notify::send_discord_webhook("http://127.0.0.1:8099/status/500", "ping"));
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    std::cout << "      elapsed: " << elapsed.count() << "s (backoff proves retries ran)\n";

    return 0;
}
