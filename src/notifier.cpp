#include "notifier.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

namespace notify {
namespace {

using nlohmann::json;

constexpr int         kMaxAttempts      = 3;
constexpr int         kRequestTimeoutMs = 10000;
constexpr std::size_t kTelegramTextMax  = 4096;   // hard limit on "text"

std::string money(double amount, const std::string& currency) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << amount << ' ' << currency;
    return out.str();
}

// Telegram parses the message as HTML, so any '&', '<' or '>' in interpolated
// data must be escaped. An unescaped one makes the API reject the whole
// message with "can't parse entities", which presents as a silently dead bot.
std::string html_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;";  break;
            case '>': out += "&gt;";  break;
            default:  out += c;       break;
        }
    }
    return out;
}

// A rate-limited request carries the wait in a Retry-After header and in a
// retry_after field nested under "parameters". Prefer the header, fall back to
// the body, and cap the wait so a bad value cannot park the loop for an hour.
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
    if (!body.is_discarded() && body.is_object() && body.contains("parameters")) {
        const json& parameters = body.at("parameters");
        if (parameters.is_object() && parameters.contains("retry_after")) {
            const json& value = parameters.at("retry_after");
            if (value.is_number()) return (std::min)(value.get<double>(), kCap);
        }
    }

    return kFallback;
}

SendResult post_json(const std::string& url, const std::string& payload) {
    SendResult result;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        const cpr::Response response = cpr::Post(
            cpr::Url{url},
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

        if (response.status_code >= 200 && response.status_code < 300) {
            // Telegram answers 200 with {"ok": true, ...}. A 2xx carrying
            // ok:false would be a silent failure, so check the body too.
            const json body =
                json::parse(response.text, nullptr, /*allow_exceptions=*/false);
            if (!body.is_discarded() && body.is_object() &&
                body.contains("ok") && body.at("ok").is_boolean() &&
                !body.at("ok").get<bool>()) {
                result.error = "API reported failure: " + response.text;
                return result;
            }

            result.ok = true;
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
            result.error =
                "server error (" + std::to_string(response.status_code) + ")";
            if (attempt < kMaxAttempts) {
                std::this_thread::sleep_for(std::chrono::seconds(attempt * 2));
                continue;
            }
            return result;
        }

        // 4xx other than 429: a revoked token, a chat the bot was removed
        // from, a malformed message. Retrying cannot help, so surface it.
        result.error = "rejected with HTTP " + std::to_string(response.status_code) +
                       (response.text.empty() ? "" : (": " + response.text));
        return result;
    }

    return result;
}

}  // namespace

std::string build_price_drop_message(const FlightOffer&     offer,
                                     const db::PriceUpdate& update) {
    // Quote the saving against the same baseline the threshold was judged on,
    // otherwise a message sent for clearing 10% could display a smaller figure
    // and read as a bug. The two are identical until a first alert is sent.
    const double baseline =
        update.alert_baseline > 0.0 ? update.alert_baseline : update.previous_lowest;
    const double saving  = baseline - update.current_price;
    const double percent = baseline > 0.0 ? (saving / baseline) * 100.0 : 0.0;

    std::ostringstream out;
    out << std::fixed << std::setprecision(0);

    // Every interpolated value goes through html_escape. All of it -- city
    // names, country, the booking link -- now comes from the API, so none of
    // it is ours to trust.
    out << "\xE2\x9C\x88\xEF\xB8\x8F <b>" << html_escape(offer.origin_city)
        << " \xE2\x86\x92 " << html_escape(offer.destination_city);
    if (!offer.destination_country.empty()) {
        out << ", " << html_escape(offer.destination_country);
    }
    out << "</b>\n";

    out << "<b>" << html_escape(money(update.current_price, offer.currency)) << "</b>"
        << "  (was " << html_escape(money(baseline, offer.currency))
        << ", save " << html_escape(money(saving, offer.currency))
        << " / " << percent << "%)\n";

    out << "\xF0\x9F\x93\x85 " << html_escape(offer.departure_date);
    if (!offer.return_date.empty()) {
        out << " \xE2\x86\x92 " << html_escape(offer.return_date)
            << "  (" << offer.nights() << " nights)";
    }
    out << '\n';

    // The record price previously belonged to different dates; showing them
    // makes it obvious the comparison is across trips, not a same-trip drop.
    if (!update.previous_departure_date.empty() &&
        update.previous_departure_date != offer.departure_date) {
        out << "previous best was "
            << html_escape(update.previous_departure_date) << '\n';
    }

    out << "<code>" << html_escape(offer.origin) << '-'
        << html_escape(offer.destination) << "</code>";

    if (!offer.booking_link.empty()) {
        out << "  <a href=\"" << html_escape(offer.booking_link) << "\">book</a>";
    }

    return out.str();
}

SendResult send_telegram_message(const Telegram&    target,
                                 const std::string& html_message) {
    SendResult result;

    if (!target.configured()) {
        result.error = "no Telegram bot token or chat id configured";
        return result;
    }

    std::string text = html_message;
    if (text.size() > kTelegramTextMax) {
        text.resize(kTelegramTextMax - 3);
        text += "...";
    }

    // Built through nlohmann rather than string concatenation, so the message
    // cannot break out of the JSON regardless of what it contains.
    const json payload = {
        {"chat_id", target.chat_id},
        {"text", text},
        {"parse_mode", "HTML"},
        // The booking deep link would otherwise render a large preview card
        // that buries the price.
        {"disable_web_page_preview", true},
    };

    // The token goes in the path -- that is how Telegram's API works -- so this
    // URL must never reach a log line.
    return post_json(target.api_base + "/bot" + target.bot_token + "/sendMessage",
                     payload.dump());
}

SendResult send_price_drop_alert(const Telegram&        target,
                                 const FlightOffer&     offer,
                                 const db::PriceUpdate& update) {
    return send_telegram_message(target, build_price_drop_message(offer, update));
}

}  // namespace notify
