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
    // The UNIQUE constraint doubles as the lookup index for record_price(),
    // so no separate CREATE INDEX is needed.
    exec(
        "CREATE TABLE IF NOT EXISTS routes ("
        "  id             INTEGER PRIMARY KEY,"
        "  origin         TEXT NOT NULL,"
        "  destination    TEXT NOT NULL,"
        "  departure_date TEXT NOT NULL,"
        "  airline        TEXT NOT NULL DEFAULT '',"
        "  currency       TEXT NOT NULL DEFAULT 'EUR',"
        "  lowest_price   REAL NOT NULL,"
        "  first_seen     TEXT NOT NULL,"
        "  last_checked   TEXT NOT NULL,"
        "  lowest_seen_at TEXT NOT NULL,"
        "  UNIQUE (origin, destination, departure_date)"
        ")");
}

std::optional<double> Database::lowest_price(
        const std::string& origin,
        const std::string& destination,
        const std::string& departure_date) const {
    Statement select(db_,
        "SELECT lowest_price FROM routes "
        "WHERE origin = ?1 AND destination = ?2 AND departure_date = ?3");

    select.bind(1, origin).bind(2, destination).bind(3, departure_date);

    if (!select.step()) return std::nullopt;
    return select.column_double(0);
}

PriceUpdate Database::record_price(const FlightOffer& offer) {
    const std::string now = iso8601_utc_now();

    PriceUpdate result;
    result.current_price = offer.price;

    Transaction txn(db_);

    // Read the historical low inside the transaction, so no concurrent writer
    // can slip a cheaper price in between the SELECT and the UPDATE.
    const std::optional<double> previous =
        lowest_price(offer.origin, offer.destination, offer.departure_date);

    if (!previous.has_value()) {
        result.first_sighting = true;

        Statement insert(db_,
            "INSERT INTO routes (origin, destination, departure_date, airline,"
            "                    currency, lowest_price, first_seen,"
            "                    last_checked, lowest_seen_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?7, ?7)");

        insert.bind(1, offer.origin)
              .bind(2, offer.destination)
              .bind(3, offer.departure_date)
              .bind(4, offer.airline)
              .bind(5, offer.currency)
              .bind(6, offer.price)
              .bind(7, now);
        insert.step();

    } else {
        result.previous_lowest = *previous;
        result.new_low         = offer.price < (*previous - kPriceEpsilon);

        if (result.new_low) {
            Statement update(db_,
                "UPDATE routes SET lowest_price = ?4, airline = ?5,"
                "                  currency = ?6, last_checked = ?7,"
                "                  lowest_seen_at = ?7 "
                "WHERE origin = ?1 AND destination = ?2 AND departure_date = ?3");

            update.bind(1, offer.origin)
                  .bind(2, offer.destination)
                  .bind(3, offer.departure_date)
                  .bind(4, offer.price)
                  .bind(5, offer.airline)
                  .bind(6, offer.currency)
                  .bind(7, now);
            update.step();
        } else {
            // Price held or rose: remember that we looked, nothing else.
            Statement touch(db_,
                "UPDATE routes SET last_checked = ?4 "
                "WHERE origin = ?1 AND destination = ?2 AND departure_date = ?3");

            touch.bind(1, offer.origin)
                 .bind(2, offer.destination)
                 .bind(3, offer.departure_date)
                 .bind(4, now);
            touch.step();
        }
    }

    txn.commit();
    return result;
}

}  // namespace db
