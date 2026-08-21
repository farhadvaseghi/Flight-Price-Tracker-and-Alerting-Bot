#pragma once

#include <optional>
#include <stdexcept>
#include <string>

#include "flight.h"

// Forward declaration keeps <sqlite3.h> out of every translation unit that
// merely wants to talk to the store.
struct sqlite3;

namespace db {

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

// What happened when one freshly fetched offer was fed into the store.
struct PriceUpdate {
    bool        first_sighting  = false;  // city pair was not in `routes` before
    bool        new_low         = false;  // beat the previously stored lowest_price
    double      previous_lowest = 0.0;    // meaningless when first_sighting is true
    double      current_price   = 0.0;
    std::string previous_departure_date;  // dates behind the old record price
    std::string previous_return_date;
};

// RAII handle on the SQLite file. Non-copyable, movable.
class Database {
public:
    // Opens (creating if needed) the database at `path`. Throws db::Error.
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // Creates or migrates the `routes` table. Safe to call on every startup.
    void initialize();

    // Records one observation and reports whether it is a new historical low.
    // Runs as a single transaction: either the row is updated and the verdict
    // is correct, or nothing changes at all.
    PriceUpdate record_price(const FlightOffer& offer);

    // The cheapest price ever seen for a city pair, or nullopt if never seen.
    std::optional<double> lowest_price(const std::string& origin,
                                       const std::string& destination) const;

    // Number of city pairs currently tracked.
    int route_count() const;

private:
    void exec(const char* sql);

    sqlite3* db_ = nullptr;
};

}  // namespace db
