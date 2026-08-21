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

}  // namespace notify
