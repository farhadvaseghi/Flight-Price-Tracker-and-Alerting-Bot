#pragma once

#include <string>

#include "database.h"
#include "flight.h"

namespace notify {

// Outcome of one webhook delivery attempt (after any internal retries).
struct SendResult {
    bool        ok          = false;
    long        status_code = 0;   // 0 when the request never reached Discord
    std::string error;             // empty on success
};

// Posts a plain-text message to a Discord webhook.
//
// The URL is a parameter rather than a global because it is a secret: it
// belongs in the environment or a gitignored config file, not compiled into
// the binary. Anyone holding it can post to the channel.
//
// Retries on 429 and 5xx with backoff. Returns rather than throws, because a
// failed alert must never take down the polling loop.
SendResult send_discord_webhook(const std::string& webhook_url,
                                const std::string& message);

// Posts a formatted price-drop embed. Falls back to nothing if the update is
// not actually an alert-worthy event -- the caller decides that.
SendResult send_price_drop_alert(const std::string&    webhook_url,
                                 const FlightOffer&    offer,
                                 const db::PriceUpdate& update);

// Renders the alert payload without sending it. Exposed so the loop's
// --dry-run mode can show exactly what would have been posted.
std::string build_price_drop_payload(const FlightOffer&     offer,
                                     const db::PriceUpdate& update);

}  // namespace notify
