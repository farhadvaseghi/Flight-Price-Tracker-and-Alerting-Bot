// Step 2 harness: exercises the SQLite persistence layer end to end.
// This file gets replaced by the real application loop in Step 5.

#include <cstdio>
#include <iostream>

#include "database.h"
#include "flight.h"

namespace {

void report(const char* label, const db::PriceUpdate& update) {
    std::cout << label << "\n"
              << "    price          : " << update.current_price << '\n'
              << "    first sighting : " << (update.first_sighting ? "yes" : "no") << '\n'
              << "    new low        : " << (update.new_low ? "YES -> alert" : "no") << '\n';
    if (!update.first_sighting) {
        std::cout << "    previous low   : " << update.previous_lowest << '\n';
    }
    std::cout << '\n';
}

FlightOffer make_offer(double price) {
    FlightOffer offer;
    offer.origin         = "FRA";
    offer.destination    = "TBS";
    offer.departure_date = "2026-11-14";
    offer.airline        = "Turkish Airlines";
    offer.currency       = "EUR";
    offer.price          = price;
    return offer;
}

}  // namespace

int main() {
    const char* kPath = "step2_test.db";
    std::remove(kPath);  // start from a clean slate every run

    try {
        db::Database store(kPath);
        store.initialize();

        report("1. first ever observation at 412.50",
               store.record_price(make_offer(412.50)));

        report("2. price rises to 480.00",
               store.record_price(make_offer(480.00)));

        report("3. price drops to 389.99",
               store.record_price(make_offer(389.99)));

        report("4. identical price seen again (389.99)",
               store.record_price(make_offer(389.99)));

        report("5. drops by a third of a cent (389.9867) - FP noise",
               store.record_price(make_offer(389.9867)));

        const auto low = store.lowest_price("FRA", "TBS", "2026-11-14");
        std::cout << "stored lowest_price : " << (low ? *low : -1.0) << '\n';

        const auto missing = store.lowest_price("FRA", "JFK", "2026-11-14");
        std::cout << "unknown route       : "
                  << (missing ? "unexpectedly present" : "nullopt (correct)") << '\n';

    } catch (const db::Error& e) {
        std::cerr << "database error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
