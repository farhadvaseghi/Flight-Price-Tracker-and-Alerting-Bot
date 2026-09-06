#pragma once

#include <string>

#include "database.h"
#include "flight.h"

namespace notify {

// Where alerts go.
//
// The bot token is a credential: anyone holding it can post as your bot and
// read what it receives. It belongs in the environment, never compiled in and
// never committed. The chat id is not secret.
struct Telegram {
    std::string bot_token;
    std::string chat_id;   // "@channelname" or a numeric id like -1001234567890

    // Overridable so the delivery path can be exercised against a local mock
    // instead of the real API. Nothing but tests should change this.
    std::string api_base = "https://api.telegram.org";

    bool configured() const { return !bot_token.empty() && !chat_id.empty(); }
};

// Outcome of one delivery attempt, after any internal retries.
struct SendResult {
    bool        ok          = false;
    long        status_code = 0;   // 0 when the request never left the process
    std::string error;             // empty on success
};

// Posts an HTML-formatted message to the configured chat. Retries on 429 and
// 5xx with backoff. Returns rather than throws: a failed alert must never take
// down the polling loop.
SendResult send_telegram_message(const Telegram& target, const std::string& html_message);

// Posts a formatted price-drop alert.
SendResult send_price_drop_alert(const Telegram&        target,
                                 const FlightOffer&     offer,
                                 const db::PriceUpdate& update);

// Renders the alert without sending it, so --dry-run can show exactly what
// would go out.
std::string build_price_drop_message(const FlightOffer&     offer,
                                     const db::PriceUpdate& update);

// One message listing the cheapest routes already stored.
//
// Exists because every route tracked before first sightings were announced was
// seeded in silence, so those fares would otherwise never be mentioned -- each
// would have to fall further before it said anything at all. `total_under_cap`
// is the full count, so the digest can admit what it left out.
//
// Speaks in IATA codes: the table holds no city names, because those arrive
// with a live offer and were never worth a column.
// Returns one or more messages, because 20 routes do not fit in one.
//
// A booking deep link is about 230 characters and Telegram caps a message at
// 4096, so a full digest runs to roughly 6000 and would be truncated -- mid-tag,
// which the HTML parser then rejects outright, losing the whole message rather
// than the tail. Splitting on a route boundary keeps every chunk valid.
std::vector<std::string> build_digest_messages(
    const std::vector<db::Database::RouteSnapshot>& routes,
    int                                             total_under_cap,
    double                                          cap);

// Sends each chunk in order, stopping at the first failure.
SendResult send_digest(const Telegram&                                 target,
                       const std::vector<db::Database::RouteSnapshot>& routes,
                       int                                             total_under_cap,
                       double                                          cap);

}  // namespace notify
