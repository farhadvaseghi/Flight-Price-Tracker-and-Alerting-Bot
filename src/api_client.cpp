#include "api_client.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace api {
namespace {

using nlohmann::json;

// Fetched data is never trusted to have the shape we expect, so every read
// below goes through one of these helpers rather than operator[] or at().

// value() would throw type_error.302 if the key existed but held null, which
// is exactly what a real API does for "no airline recorded". Check the type.
std::string string_or(const json& node, const char* key, const std::string& fallback) {
    if (!node.is_object() || !node.contains(key)) return fallback;
    const json& value = node.at(key);
    return value.is_string() ? value.get<std::string>() : fallback;
}

// Prices arrive as a JSON number from some APIs and as a decimal string from
// others ("412.50"). Accept both, reject anything else.
std::optional<double> to_price(const json& value) {
    if (value.is_number()) {
        return value.get<double>();
    }

    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        try {
            // stod reads the decimal point per the current C locale, which we
            // never touch, so it stays "C" and '.' is correct.
            std::size_t  consumed = 0;
            const double parsed   = std::stod(text, &consumed);

            // Reject "412.50 EUR" and friends: a partial parse is a red flag,
            // not a success.
            if (consumed != text.size()) return std::nullopt;
            return parsed;
        } catch (const std::exception&) {
            return std::nullopt;  // "n/a", "", "abc"
        }
    }

    return std::nullopt;
}

}  // namespace

std::string fetch_flight_data() {
    // MOCK RESPONSE.
    //
    // To go live, replace the body of this function with:
    //
    //     const cpr::Response r = cpr::Get(
    //         cpr::Url{"https://api.example.com/v1/flight-offers"},
    //         cpr::Parameters{{"origin", "FRA"}, {"destination", "TBS"}},
    //         cpr::Header{{"Authorization", "Bearer " + api_key}},
    //         cpr::Timeout{10000});
    //     if (r.error || r.status_code != 200) return "";
    //     return r.text;
    //
    // Everything downstream of here already copes with "" and with garbage,
    // so no other file has to change.
    //
    // The payload below is deliberately messy: entries 4, 5 and 6 are the
    // kinds of malformed record a real API will eventually hand you.
    return R"JSON({
  "meta": {
    "currency": "EUR",
    "count": 6
  },
  "offers": [
    {
      "id": "of_001",
      "origin": "FRA",
      "destination": "TBS",
      "departure_date": "2026-11-14",
      "airline": "Turkish Airlines",
      "price": { "amount": "412.50", "currency": "EUR" }
    },
    {
      "id": "of_002",
      "origin": "FRA",
      "destination": "TBS",
      "departure_date": "2026-12-03",
      "airline": "Lufthansa",
      "price": { "amount": 389.99, "currency": "EUR" }
    },
    {
      "id": "of_003",
      "origin": "BER",
      "destination": "IST",
      "departure_date": "2026-11-14",
      "airline": null,
      "price": { "amount": "198.00" }
    },
    {
      "id": "of_004",
      "origin": "MUC",
      "destination": "DXB",
      "departure_date": "2026-10-02",
      "airline": "Emirates"
    },
    {
      "id": "of_005",
      "origin": "HAM",
      "destination": "LIS",
      "departure_date": "2026-09-30",
      "airline": "TAP",
      "price": { "amount": "n/a", "currency": "EUR" }
    },
    {
      "id": "of_006",
      "destination": "CDG",
      "departure_date": "2026-11-01",
      "airline": "Air France",
      "price": { "amount": "150.00", "currency": "EUR" }
    }
  ]
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

    if (!root.contains("offers") || !root.at("offers").is_array()) {
        result.problems.emplace_back("response has no 'offers' array");
        return result;
    }

    // A currency at the top level is the fallback for offers that omit it.
    const std::string default_currency =
        string_or(root.value("meta", json::object()), "currency", "EUR");

    std::size_t index = 0;
    for (const json& entry : root.at("offers")) {
        const std::string label =
            "offer[" + std::to_string(index++) + "] " + string_or(entry, "id", "<no id>");

        if (!entry.is_object()) {
            result.problems.emplace_back(label + ": not an object");
            continue;
        }

        FlightOffer offer;
        offer.origin         = string_or(entry, "origin", "");
        offer.destination    = string_or(entry, "destination", "");
        offer.departure_date = string_or(entry, "departure_date", "");
        offer.airline        = string_or(entry, "airline", "Unknown");

        // These three form the primary key of the routes table, so an entry
        // missing any of them cannot be stored at all.
        if (offer.origin.empty() || offer.destination.empty() ||
            offer.departure_date.empty()) {
            result.problems.emplace_back(label + ": missing origin, destination or date");
            continue;
        }

        if (!entry.contains("price") || !entry.at("price").is_object()) {
            result.problems.emplace_back(label + ": no price object");
            continue;
        }

        const json& price_node = entry.at("price");
        offer.currency = string_or(price_node, "currency", default_currency);

        if (!price_node.contains("amount")) {
            result.problems.emplace_back(label + ": price has no amount");
            continue;
        }

        const std::optional<double> amount = to_price(price_node.at("amount"));
        if (!amount.has_value()) {
            result.problems.emplace_back(label + ": price amount is not a number");
            continue;
        }

        // A zero or negative fare is a data bug, not a bargain worth alerting on.
        if (*amount <= 0.0) {
            result.problems.emplace_back(label + ": non-positive price");
            continue;
        }

        offer.price = *amount;
        result.offers.push_back(std::move(offer));
    }

    return result;
}

}  // namespace api
