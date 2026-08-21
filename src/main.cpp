// Step 3 harness: parses the mock API response and feeds it into the store.
// This file gets replaced by the real application loop in Step 5.

#include <cstdio>
#include <iomanip>
#include <iostream>

#include "api_client.h"
#include "database.h"
#include "flight.h"

int main() {
    const char* kPath = "step3_test.db";
    std::remove(kPath);

    std::cout << std::fixed << std::setprecision(2);

    try {
        db::Database store(kPath);
        store.initialize();

        // --- pass 1: cold database, everything is a first sighting ---
        const std::string    payload = api::fetch_flight_data();
        const api::ParseResult parsed = api::parse_flight_offers(payload);

        std::cout << "parsed " << parsed.offers.size() << " usable offers, "
                  << parsed.problems.size() << " rejected\n\n";

        std::cout << "rejected entries:\n";
        for (const std::string& problem : parsed.problems) {
            std::cout << "  - " << problem << '\n';
        }
        std::cout << '\n';

        std::cout << "pass 1 (cold database):\n";
        for (const FlightOffer& offer : parsed.offers) {
            const db::PriceUpdate update = store.record_price(offer);
            std::cout << "  " << offer.origin << "->" << offer.destination
                      << ' ' << offer.departure_date
                      << "  " << offer.price << ' ' << offer.currency
                      << "  [" << offer.airline << "]"
                      << (update.first_sighting ? "  (new route)" : "")
                      << (update.new_low ? "  (NEW LOW)" : "") << '\n';
        }

        // --- pass 2: same data again, so nothing should look like a drop ---
        std::cout << "\npass 2 (identical data replayed):\n";
        for (const FlightOffer& offer : parsed.offers) {
            const db::PriceUpdate update = store.record_price(offer);
            std::cout << "  " << offer.origin << "->" << offer.destination
                      << "  first_sighting=" << (update.first_sighting ? "y" : "n")
                      << "  new_low=" << (update.new_low ? "y" : "n") << '\n';
        }

        // --- pass 3: one genuine drop ---
        std::cout << "\npass 3 (FRA->TBS 2026-11-14 drops to 355.00):\n";
        FlightOffer cheaper = parsed.offers.front();
        cheaper.price = 355.00;
        const db::PriceUpdate update = store.record_price(cheaper);
        std::cout << "  previous low " << update.previous_lowest
                  << " -> " << update.current_price
                  << "  new_low=" << (update.new_low ? "YES" : "no") << '\n';

        // --- malformed input must not throw ---
        std::cout << "\nrobustness:\n";
        for (const char* bad : {"", "<html>502 Bad Gateway</html>", "{\"offers\": 42}",
                                "{\"offers\": [null, 7]}"}) {
            const api::ParseResult r = api::parse_flight_offers(bad);
            std::cout << "  " << std::setw(30) << std::left
                      << (std::string(bad).empty() ? "(empty body)" : bad)
                      << " -> " << r.offers.size() << " offers, "
                      << r.problems.size() << " problems\n";
        }

    } catch (const db::Error& e) {
        std::cerr << "database error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
