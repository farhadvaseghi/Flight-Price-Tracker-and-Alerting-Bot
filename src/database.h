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

    // What the "how much cheaper" percentage is measured against.
    //
    // This is the price we last alerted at, not the stored record, and the two
    // diverge on purpose. Every new low overwrites lowest_price whether or not
    // it was worth a message, so measuring against it would compare each drop
    // only with the drop before it: a fare sliding 3% per sweep would clear no
    // threshold, ever, while quietly losing a third of its price. Measuring
    // against the last alert instead makes those small steps accumulate, so the
    // threshold means "cheaper than when you last heard from me" -- which is
    // what a person setting one actually wants.
    //
    // Falls back to previous_lowest until a first alert is sent.
    double alert_baseline = 0.0;
    double drop_percent   = 0.0;   // 0 when there is nothing to compare against
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

    // Records that an alert went out for this city pair at `price`, so the next
    // drop is measured from here rather than from the record low. Call it only
    // when a message actually reached the user -- a failed send must not move
    // the baseline, or the alert it failed to deliver is lost for good.
    void mark_alerted(const std::string& origin,
                      const std::string& destination,
                      double             price);

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
