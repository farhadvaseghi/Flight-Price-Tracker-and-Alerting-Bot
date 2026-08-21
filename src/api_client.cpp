#include "api_client.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>

#include "geo.h"

namespace api {
namespace {

using nlohmann::json;

constexpr int kTokenTimeoutMs  = 10000;
constexpr int kSearchTimeoutMs = 20000;

// Fetched data is never trusted to have the shape we expect, so every read
// below goes through one of these helpers rather than operator[] or at().

// value() would throw type_error.302 if the key existed but held null, which
// is exactly what an API does for a field it has no data for. Check the type.
std::string string_or(const json& node, const char* key, const std::string& fallback) {
    if (!node.is_object() || !node.contains(key)) return fallback;
    const json& value = node.at(key);
    return value.is_string() ? value.get<std::string>() : fallback;
}

// Amadeus quotes prices as decimal strings ("142.30"); other providers use
// JSON numbers. Accept both, reject anything else.
std::optional<double> to_price(const json& value) {
    if (value.is_number()) return value.get<double>();

    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        try {
            // stod reads the decimal point per the current C locale, which we
            // never touch, so it stays "C" and '.' is correct.
            std::size_t  consumed = 0;
            const double parsed   = std::stod(text, &consumed);
            // Reject "142.30 EUR" and friends: a partial parse is a red flag.
            if (consumed != text.size()) return std::nullopt;
            return parsed;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

}  // namespace

std::string access_token(const Credentials& creds) {
    if (!creds.present()) return "";

    // Amadeus tokens live around 30 minutes. Cache across calls so a cycle
    // sweeping a dozen origins authenticates once rather than a dozen times.
    static std::string                             cached;
    static std::chrono::steady_clock::time_point   expiry;

    if (!cached.empty() && std::chrono::steady_clock::now() < expiry) return cached;

    const cpr::Response response = cpr::Post(
        cpr::Url{"https://" + creds.host + "/v1/security/oauth2/token"},
        cpr::Payload{{"grant_type", "client_credentials"},
                     {"client_id", creds.client_id},
                     {"client_secret", creds.client_secret}},
        cpr::Timeout{kTokenTimeoutMs});

    if (response.error || response.status_code != 200) return "";

    const json body = json::parse(response.text, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded() || !body.is_object()) return "";

    const std::string token = string_or(body, "access_token", "");
    if (token.empty()) return "";

    // Refresh a minute early rather than racing the expiry mid-cycle.
    int lifetime = 1799;
    if (body.contains("expires_in") && body.at("expires_in").is_number_integer()) {
        lifetime = body.at("expires_in").get<int>();
    }

    cached = token;
    expiry = std::chrono::steady_clock::now() + std::chrono::seconds(lifetime - 60);
    return cached;
}

std::string fetch_flight_data(const Credentials& creds, const SearchParams& params) {
    const std::string token = access_token(creds);
    if (token.empty()) return "";

    // Flight Inspiration Search: one call returns every destination reachable
    // from this origin with a price, which is what makes "anywhere in Europe"
    // affordable on a free quota. Querying city pairs individually would be
    // hundreds of calls per cycle.
    cpr::Parameters query{
        {"origin", params.origin},
        {"departureDate", params.departure_window},
        {"currencyCode", params.currency},
        {"oneWay", "false"},
        {"nonStop", "false"},
        {"duration", std::to_string(params.min_nights) + "," +
                     std::to_string(params.max_nights)},
    };

    if (params.max_price > 0.0) {
        // maxPrice must be an integer; rounding down keeps the cap honest.
        query.Add({"maxPrice", std::to_string(static_cast<int>(params.max_price))});
    }

    const cpr::Response response = cpr::Get(
        cpr::Url{"https://" + creds.host + "/v1/shopping/flight-destinations"},
        query,
        cpr::Header{{"Authorization", "Bearer " + token}},
        cpr::Timeout{kSearchTimeoutMs});

    if (response.error || response.status_code != 200) return "";
    return response.text;
}

std::string mock_flight_data() {
    // Shaped exactly like a Flight Inspiration Search response, including the
    // kinds of malformed entry a real API eventually hands you: entry 4 has no
    // price, entry 5's price is unparseable, entry 6 has no destination.
    // JFK is present to prove the region filter drops it.
    return R"JSON({
  "data": [
    {
      "type": "flight-destination",
      "origin": "FRA",
      "destination": "IST",
      "departureDate": "2026-09-18",
      "returnDate": "2026-09-21",
      "price": { "total": "142.30" },
      "links": { "flightOffers": "https://test.api.amadeus.com/v2/shopping/flight-offers?originLocationCode=FRA" }
    },
    {
      "type": "flight-destination",
      "origin": "FRA",
      "destination": "LIS",
      "departureDate": "2026-10-02",
      "returnDate": "2026-10-05",
      "price": { "total": 88.99 }
    },
    {
      "type": "flight-destination",
      "origin": "FRA",
      "destination": "JFK",
      "departureDate": "2026-09-25",
      "returnDate": "2026-09-28",
      "price": { "total": "310.00" }
    },
    {
      "type": "flight-destination",
      "origin": "FRA",
      "destination": "ATH",
      "departureDate": "2026-11-06",
      "returnDate": "2026-11-09"
    },
    {
      "type": "flight-destination",
      "origin": "FRA",
      "destination": "BCN",
      "departureDate": "2026-09-11",
      "returnDate": "2026-09-14",
      "price": { "total": "n/a" }
    },
    {
      "type": "flight-destination",
      "origin": "FRA",
      "departureDate": "2026-10-20",
      "returnDate": "2026-10-23",
      "price": { "total": "75.00" }
    }
  ],
  "meta": { "currency": "EUR" }
})JSON";
}

ParseResult parse_flight_offers(const std::string& payload) {
    ParseResult result;

    if (payload.empty()) {
        result.problems.emplace_back("empty response body");
        return result;
    }

    // allow_exceptions = false: a truncated or non-JSON body (an HTML error
    // page from a proxy, say) yields a discarded value instead of throwing.
    const json root = json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        result.problems.emplace_back("response body is not valid JSON");
        return result;
    }

    if (!root.is_object() || !root.contains("data") || !root.at("data").is_array()) {
        result.problems.emplace_back("response has no 'data' array");
        return result;
    }

    const std::string default_currency =
        string_or(root.value("meta", json::object()), "currency", "EUR");

    std::size_t index = 0;
    for (const json& entry : root.at("data")) {
        const std::string label = "data[" + std::to_string(index++) + "]";

        if (!entry.is_object()) {
            result.problems.emplace_back(label + ": not an object");
            continue;
        }

        FlightOffer offer;
        offer.origin         = string_or(entry, "origin", "");
        offer.destination    = string_or(entry, "destination", "");
        offer.departure_date = string_or(entry, "departureDate", "");
        offer.return_date    = string_or(entry, "returnDate", "");
        offer.currency       = default_currency;
        offer.booking_link   = string_or(entry.value("links", json::object()),
                                         "flightOffers", "");

        // Origin and destination form the primary key of the routes table, so
        // an entry missing either cannot be stored at all.
        if (offer.origin.empty() || offer.destination.empty()) {
            result.problems.emplace_back(label + ": missing origin or destination");
            continue;
        }

        // Anywhere outside Europe or Turkey is not an error, just not ours.
        // Counted rather than logged, since it is the common case.
        if (!geo::in_region(offer.destination)) {
            ++result.filtered;
            continue;
        }

        if (!entry.contains("price") || !entry.at("price").is_object()) {
            result.problems.emplace_back(label + " " + offer.destination + ": no price");
            continue;
        }

        const json& price_node = entry.at("price");
        if (!price_node.contains("total")) {
            result.problems.emplace_back(label + " " + offer.destination + ": price has no total");
            continue;
        }

        const std::optional<double> amount = to_price(price_node.at("total"));
        if (!amount.has_value()) {
            result.problems.emplace_back(label + " " + offer.destination + ": price is not a number");
            continue;
        }

        // A zero or negative fare is a data bug, not a bargain worth alerting on.
        if (*amount <= 0.0) {
            result.problems.emplace_back(label + " " + offer.destination + ": non-positive price");
            continue;
        }

        offer.price = *amount;
        result.offers.push_back(std::move(offer));
    }

    return result;
}

}  // namespace api
