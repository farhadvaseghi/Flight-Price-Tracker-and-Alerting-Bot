#include "notifier.h"

#include "geo.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <thread>

namespace notify {
namespace {

using nlohmann::json;

constexpr int         kMaxAttempts       = 3;
constexpr int         kRequestTimeoutMs  = 10000;
constexpr std::size_t kDiscordContentMax = 2000;   // hard limit on "content"
constexpr int         kColourGreen       = 0x2ECC71;

std::string money(double amount, const std::string& currency) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << amount << ' ' << currency;
    return out.str();
}

// Discord answers a rate-limited request with a Retry-After header and a
// retry_after field in the body. Prefer the header, fall back to the body,
// and cap the wait so a bad value cannot park the loop for an hour.
double retry_delay_seconds(const cpr::Response& response) {
    constexpr double kFallback = 2.0;
    constexpr double kCap      = 60.0;

    const auto header = response.header.find("retry-after");
    if (header != response.header.end()) {
        try {
            // Parenthesised so a windows.h min() macro cannot expand this.
            // NOMINMAX is set for our targets too, but this stays correct even
            // for someone compiling these sources without it.
            return (std::min)(std::stod(header->second), kCap);
        } catch (const std::exception&) {
            // fall through to the body
        }
    }

    const json body = json::parse(response.text, nullptr, /*allow_exceptions=*/false);
    if (!body.is_discarded() && body.is_object() && body.contains("retry_after")) {
        const json& value = body.at("retry_after");
        if (value.is_number()) return (std::min)(value.get<double>(), kCap);
    }

    return kFallback;
}

SendResult post_json(const std::string& webhook_url, const std::string& payload) {
    SendResult result;

    if (webhook_url.empty()) {
        result.error = "no webhook URL configured";
        return result;
    }

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        const cpr::Response response = cpr::Post(
            cpr::Url{webhook_url},
            cpr::Header{{"Content-Type", "application/json"}},
            cpr::Body{payload},
            cpr::Timeout{kRequestTimeoutMs});

        result.status_code = response.status_code;

        // Transport-level failure: DNS, TLS, timeout. Worth retrying.
        if (response.error) {
            result.error = "request failed: " + response.error.message;
            if (attempt < kMaxAttempts) {
                std::this_thread::sleep_for(std::chrono::seconds(attempt * 2));
                continue;
            }
            return result;
        }

        // Discord acknowledges a webhook with 204 No Content, NOT 200. Add
        // ?wait=true to the URL and it answers 200 with the created message,
        // so accept the whole 2xx range rather than one magic number.
        if (response.status_code >= 200 && response.status_code < 300) {
            result.ok    = true;
            result.error.clear();
            return result;
        }

        if (response.status_code == 429) {
            const double delay = retry_delay_seconds(response);
            result.error = "rate limited (429)";
            if (attempt < kMaxAttempts) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(static_cast<long long>(delay * 1000)));
                continue;
            }
            return result;
        }

        if (response.status_code >= 500) {
            result.error = "Discord server error (" + std::to_string(response.status_code) + ")";
            if (attempt < kMaxAttempts) {
                std::this_thread::sleep_for(std::chrono::seconds(attempt * 2));
                continue;
            }
            return result;
        }

        // 4xx other than 429: a deleted webhook, a malformed payload, a bad
        // token. Retrying cannot help, so surface it immediately.
        result.error = "rejected with HTTP " + std::to_string(response.status_code) +
                       (response.text.empty() ? "" : (": " + response.text));
        return result;
    }

    return result;
}

}  // namespace

SendResult send_discord_webhook(const std::string& webhook_url,
                                const std::string& message) {
    std::string content = message;
    if (content.size() > kDiscordContentMax) {
        content.resize(kDiscordContentMax - 3);
        content += "...";
    }

    // Built through nlohmann rather than string concatenation, so an airline
    // name containing a quote or a newline cannot break the payload.
    const json payload = {{"content", content}};
    return post_json(webhook_url, payload.dump());
}

std::string build_price_drop_payload(const FlightOffer&     offer,
                                     const db::PriceUpdate& update) {
    const double saving = update.previous_lowest - update.current_price;
    const double percent =
        update.previous_lowest > 0.0 ? (saving / update.previous_lowest) * 100.0 : 0.0;

    std::ostringstream percent_text;
    percent_text << std::fixed << std::setprecision(0) << percent << '%';

    const geo::Place from = geo::lookup(offer.origin);
    const geo::Place to   = geo::lookup(offer.destination);

    std::ostringstream dates;
    dates << offer.departure_date;
    if (!offer.return_date.empty()) {
        dates << "  to  " << offer.return_date
              << "  (" << offer.nights() << " nights)";
    }

    json fields = json::array({
        {{"name", "New price"}, {"value", money(update.current_price, offer.currency)}, {"inline", true}},
        {{"name", "Previous"},  {"value", money(update.previous_lowest, offer.currency)}, {"inline", true}},
        {{"name", "You save"},  {"value", money(saving, offer.currency) + "  (" + percent_text.str() + ")"}, {"inline", true}},
        {{"name", "Dates"},     {"value", dates.str()}, {"inline", false}},
    });

    // The record price previously belonged to different dates; showing them
    // makes it obvious the comparison is across trips, not a same-trip drop.
    if (!update.previous_departure_date.empty() &&
        update.previous_departure_date != offer.departure_date) {
        fields.push_back({{"name", "Previous best was"},
                          {"value", update.previous_departure_date},
                          {"inline", false}});
    }

    json embed = {
        {"title", from.city + " to " + to.city +
                  (to.country.empty() ? "" : ", " + to.country)},
        {"description", offer.origin + " to " + offer.destination},
        {"color", kColourGreen},
        {"fields", fields},
        {"footer", {{"text", "flight-price-tracker"}}},
    };

    if (!offer.booking_link.empty()) embed["url"] = offer.booking_link;

    const json payload = {
        {"username", "Flight Tracker"},
        {"embeds", json::array({embed})},
    };

    return payload.dump();
}

SendResult send_price_drop_alert(const std::string&     webhook_url,
                                 const FlightOffer&     offer,
                                 const db::PriceUpdate& update) {
    return post_json(webhook_url, build_price_drop_payload(offer, update));
}

}  // namespace notify
