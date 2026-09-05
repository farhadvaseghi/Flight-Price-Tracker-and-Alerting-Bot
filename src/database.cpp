#include "database.h"

#include <sqlite3.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace db {
namespace {

// Prices are floating point, so demand a real drop rather than rounding noise.
// Half a cent is below any currency's smallest unit.
constexpr double kPriceEpsilon = 0.005;

std::string iso8601_utc_now() {
    const auto        now = std::chrono::system_clock::now();
    const std::time_t t   = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

// RAII wrapper around a prepared statement. Everything that touches fetched
// data goes through bind() -- we never concatenate values into SQL.
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw Error(std::string("prepare failed: ") + sqlite3_errmsg(db));
        }
    }

    ~Statement() { sqlite3_finalize(stmt_); }

    Statement(const Statement&)            = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int index, const std::string& value) {
        // SQLITE_TRANSIENT: SQLite copies the bytes, so `value` is free to die
        // before step() runs.
        check(sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT));
        return *this;
    }

    Statement& bind(int index, double value) {
        check(sqlite3_bind_double(stmt_, index, value));
        return *this;
    }

    // Returns true when a row is available, false when the statement is done.
    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW)  return true;
        if (rc == SQLITE_DONE) return false;
        throw Error(std::string("step failed: ") + sqlite3_errmsg(db_));
    }

    double column_double(int index) const {
        return sqlite3_column_double(stmt_, index);
    }

    // A NULL column yields a null pointer, which std::string cannot be built
    // from, so map it to an empty string rather than crashing.
    std::string column_text(int index) const {
        const unsigned char* text = sqlite3_column_text(stmt_, index);
        return text ? reinterpret_cast<const char*>(text) : std::string();
    }

private:
    void check(int rc) const {
        if (rc != SQLITE_OK) {
            throw Error(std::string("bind failed: ") + sqlite3_errmsg(db_));
        }
    }

    sqlite3*      db_   = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

// Rolls back automatically unless commit() is reached, so a throw halfway
// through record_price() cannot leave a half-applied update behind.
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) {
        run("BEGIN IMMEDIATE", /*throw_on_error=*/true);
    }

    ~Transaction() {
        if (!finished_) run("ROLLBACK", /*throw_on_error=*/false);
    }

    Transaction(const Transaction&)            = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        run("COMMIT", /*throw_on_error=*/true);
        finished_ = true;
    }

private:
    void run(const char* sql, bool throw_on_error) {
        char*     err = nullptr;
        const int rc  = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK && throw_on_error) {
            const std::string message = err ? err : "unknown error";
            sqlite3_free(err);
            throw Error(std::string(sql) + " failed: " + message);
        }
        sqlite3_free(err);
    }

    sqlite3* db_       = nullptr;
    bool     finished_ = false;
};

}  // namespace

Database::Database(const std::string& path) {
    const int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;

    if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        // db_ is non-null even on failure -- we still have to close it.
        const std::string message =
            db_ ? sqlite3_errmsg(db_) : "could not allocate database handle";
        sqlite3_close(db_);
        db_ = nullptr;
        throw Error("cannot open " + path + ": " + message);
    }

    // Wait rather than fail if another process holds the write lock.
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode = WAL");
    exec("PRAGMA synchronous  = NORMAL");
    exec("PRAGMA foreign_keys = ON");
}

Database::~Database() {
    sqlite3_close(db_);
}

Database::Database(Database&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        sqlite3_close(db_);
        db_       = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

void Database::exec(const char* sql) {
    char*     err = nullptr;
    const int rc  = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        const std::string message = err ? err : "unknown error";
        sqlite3_free(err);
        throw Error(std::string(sql) + " failed: " + message);
    }
    sqlite3_free(err);
}


void Database::initialize() {
    // The route key is the city pair, not the pair plus a date: the question
    // this bot answers is "is FRA-IST cheaper than it has ever been", and the
    // dates are the answer rather than part of the question. Keying on the
    // date too would create thousands of rows that each need their own price
    // history before they could ever alert.
    //
    // user_version tracks the schema so an older database is recognised
    // instead of failing later with a confusing "no such column".
    int current = 0;
    {
        Statement version(db_, "PRAGMA user_version");
        if (version.step()) current = static_cast<int>(version.column_double(0));
    }

    if (current == 1) {
        // v1 keyed on (origin, destination, departure_date). There is no
        // meaningful way to fold those rows into the new key, and the data is
        // only ever a price cache, so start the history over.
        exec("DROP TABLE IF EXISTS routes");
    }

    exec(
        "CREATE TABLE IF NOT EXISTS routes ("
        "  id             INTEGER PRIMARY KEY,"
        "  origin         TEXT NOT NULL,"
        "  destination    TEXT NOT NULL,"
        "  lowest_price   REAL NOT NULL,"
        "  currency       TEXT NOT NULL DEFAULT 'EUR',"
        "  departure_date TEXT NOT NULL DEFAULT '',"   // dates behind the record
        "  return_date    TEXT NOT NULL DEFAULT '',"
        "  booking_link   TEXT NOT NULL DEFAULT '',"
        "  first_seen     TEXT NOT NULL,"
        "  last_checked   TEXT NOT NULL,"
        "  lowest_seen_at TEXT NOT NULL,"
        "  alert_baseline REAL NOT NULL DEFAULT 0,"       // what drops are measured from
        "  UNIQUE (origin, destination)"
        ")");

    // v2 -> v3 adds alert_baseline. Unlike the v1 break above this is a
    // widening change, so the rows are migrated rather than discarded: the
    // price history is months of observations that cannot be re-fetched.
    //
    // Existing rows are seeded from lowest_price. Leaving them at 0 would be
    // wrong in a way that is easy to miss: the baseline would fall back to the
    // record, which moves on every new low, and the accumulation this column
    // exists to provide would not start working until the first alert fired.
    if (current == 2) {
        exec("ALTER TABLE routes ADD COLUMN alert_baseline REAL NOT NULL DEFAULT 0");
        exec("UPDATE routes SET alert_baseline = lowest_price");
    }

    exec("PRAGMA user_version = 3");
}

std::optional<double> Database::lowest_price(const std::string& origin,
                                             const std::string& destination) const {
    Statement select(db_,
        "SELECT lowest_price FROM routes WHERE origin = ?1 AND destination = ?2");
    select.bind(1, origin).bind(2, destination);

    if (!select.step()) return std::nullopt;
    return select.column_double(0);
}

void Database::mark_alerted(const std::string& origin,
                            const std::string& destination,
                            double             price) {
    Statement update(db_,
        "UPDATE routes SET alert_baseline = ?3 "
        "WHERE origin = ?1 AND destination = ?2");
    update.bind(1, origin).bind(2, destination).bind(3, price);
    update.step();
}

int Database::route_count() const {
    Statement count(db_, "SELECT COUNT(*) FROM routes");
    return count.step() ? static_cast<int>(count.column_double(0)) : 0;
}

PriceUpdate Database::record_price(const FlightOffer& offer) {
    const std::string now = iso8601_utc_now();

    PriceUpdate result;
    result.current_price = offer.price;

    Transaction txn(db_);

    // Read the existing record inside the transaction, so no concurrent writer
    // can slip a cheaper price in between the SELECT and the UPDATE. The read
    // is scoped so its cursor is finalised before we write to the same table.
    bool   exists         = false;
    double stored_baseline = 0.0;
    {
        Statement select(db_,
            "SELECT lowest_price, departure_date, return_date, alert_baseline "
            "FROM routes WHERE origin = ?1 AND destination = ?2");
        select.bind(1, offer.origin).bind(2, offer.destination);

        exists = select.step();
        if (exists) {
            result.previous_lowest         = select.column_double(0);
            result.previous_departure_date = select.column_text(1);
            result.previous_return_date    = select.column_text(2);
            stored_baseline                = select.column_double(3);
        }
    }

    if (!exists) {
        result.first_sighting = true;

        Statement insert(db_,
            // alert_baseline starts at the first price ever seen (?3 again), so
            // a route that only ever drifts downwards still accumulates toward
            // the threshold rather than waiting for a first alert to set it.
            "INSERT INTO routes (origin, destination, lowest_price, currency,"
            "                    departure_date, return_date, booking_link,"
            "                    first_seen, last_checked, lowest_seen_at,"
            "                    alert_baseline) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?8, ?8, ?3)");

        insert.bind(1, offer.origin)
              .bind(2, offer.destination)
              .bind(3, offer.price)
              .bind(4, offer.currency)
              .bind(5, offer.departure_date)
              .bind(6, offer.return_date)
              .bind(7, offer.booking_link)
              .bind(8, now);
        insert.step();

    } else {
        result.new_low = offer.price < (result.previous_lowest - kPriceEpsilon);

        // The baseline is pinned until an alert moves it, so repeated small
        // declines add up instead of resetting the yardstick every time the
        // record falls. The fallback covers rows written before the column
        // existed. See PriceUpdate::alert_baseline.
        result.alert_baseline =
            stored_baseline > 0.0 ? stored_baseline : result.previous_lowest;

        if (result.alert_baseline > 0.0) {
            result.drop_percent =
                ((result.alert_baseline - offer.price) / result.alert_baseline) * 100.0;
        }

        if (result.new_low) {
            // The dates move with the price: the record belongs to whichever
            // trip achieved it.
            Statement update(db_,
                "UPDATE routes SET lowest_price = ?3, currency = ?4,"
                "                  departure_date = ?5, return_date = ?6,"
                "                  booking_link = ?7, last_checked = ?8,"
                "                  lowest_seen_at = ?8 "
                "WHERE origin = ?1 AND destination = ?2");

            update.bind(1, offer.origin)
                  .bind(2, offer.destination)
                  .bind(3, offer.price)
                  .bind(4, offer.currency)
                  .bind(5, offer.departure_date)
                  .bind(6, offer.return_date)
                  .bind(7, offer.booking_link)
                  .bind(8, now);
            update.step();
        } else {
            // Price held or rose: remember that we looked, nothing else.
            Statement touch(db_,
                "UPDATE routes SET last_checked = ?3 "
                "WHERE origin = ?1 AND destination = ?2");
            touch.bind(1, offer.origin).bind(2, offer.destination).bind(3, now);
            touch.step();
        }
    }

    txn.commit();
    return result;
}

}  // namespace db
