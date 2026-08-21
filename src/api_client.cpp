#include "api_client.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <optional>
#include <string>

#include "geo.h"

namespace api {
namespace {

using nlohmann::json;

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

// Prices come back as JSON numbers here, but other providers quote them as
// decimal strings, so accept both and reject anything else.
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

// departure_at is a full ISO-8601 timestamp ("2026-09-18T10:25:00Z"), but the
// routes table and every filter work in whole days. Take the date part, and
// only if it actually looks like one.
std::string date_part(const std::string& timestamp) {
    if (timestamp.size() < 10) return "";
    const std::string date = timestamp.substr(0, 10);
    if (date[4] != '-' || date[7] != '-') return "";
    return date;
}

}  // namespace

std::string fetch_flight_data(const Credentials& creds, const SearchParams& params) {
    if (!creds.present()) return "";

    // city-directions returns the cheapest cached fare to every destination
    // reachable from this origin, in one call. That is what makes "anywhere in
    // Europe" affordable: eight origins is eight requests, not hundreds.
    const cpr::Response response = cpr::Get(
        cpr::Url{"https://" + creds.host + "/v1/city-directions"},
        cpr::Parameters{{"origin", params.origin},
                        {"currency", params.currency},
                        {"token", creds.token}},
        // The response is sizeable JSON and the docs ask for compression.
        cpr::Header{{"Accept-Encoding", "gzip, deflate"}},
        cpr::Timeout{kSearchTimeoutMs});

    if (response.error || response.status_code != 200) return "";
    return response.text;
}

std::string mock_flight_data() {
    // Shaped like a real city-directions response: `data` is an object keyed by
    // destination IATA, not an array.
    //
    // Dates are relative to today rather than hardcoded. A fixed date would
    // drift out of the search window as time passed, and the mock would
    // silently start failing every filter -- taking the tests with it.
    const std::string dep_30 = date_offset_utc(30);
    const std::string ret_33 = date_offset_utc(33);
    const std::string dep_45 = date_offset_utc(45);
    const std::string ret_48 = date_offset_utc(48);
    const std::string dep_60 = date_offset_utc(60);
    const std::string ret_74 = date_offset_utc(74);   // 14 nights: too long
    const std::string dep_far = date_offset_utc(300); // outside the window

    // Entries, in order: a good one, a cheaper good one, one outside the
    // region, one whose trip is too long, one departing too far out, one with
    // an unparseable price, and one missing its destination.
    return std::string(R"JSON({
  "success": true,
  "data": {
    "IST": {"origin":"FRA","destination":"IST","price":142.3,"transfers":0,
            "airline":"TK","flight_number":"1596",
            "departure_at":")JSON") + dep_30 + R"JSON(T10:25:00Z","return_at":")JSON" + ret_33 + R"JSON(T14:05:00Z",
            "expires_at":")JSON" + date_offset_utc(3) + R"JSON(T10:00:00Z"},
    "LIS": {"origin":"FRA","destination":"LIS","price":88.99,"transfers":1,
            "airline":"TP","flight_number":"577",
            "departure_at":")JSON" + dep_45 + R"JSON(T06:00:00Z","return_at":")JSON" + ret_48 + R"JSON(T20:30:00Z",
            "expires_at":")JSON" + date_offset_utc(3) + R"JSON(T10:00:00Z"},
    "JFK": {"origin":"FRA","destination":"JFK","price":310.0,"transfers":0,
            "airline":"LH","flight_number":"400",
            "departure_at":")JSON" + dep_30 + R"JSON(T09:00:00Z","return_at":")JSON" + ret_33 + R"JSON(T18:00:00Z"},
    "AYT": {"origin":"FRA","destination":"AYT","price":95.0,"transfers":0,
            "airline":"XQ","flight_number":"981",
            "departure_at":")JSON" + dep_60 + R"JSON(T05:40:00Z","return_at":")JSON" + ret_74 + R"JSON(T22:10:00Z"},
    "ATH": {"origin":"FRA","destination":"ATH","price":77.0,"transfers":0,
            "airline":"A3","flight_number":"821",
            "departure_at":")JSON" + dep_far + R"JSON(T07:15:00Z","return_at":")JSON" + date_offset_utc(303) + R"JSON(T19:00:00Z"},
    "BCN": {"origin":"FRA","destination":"BCN","price":"n/a","transfers":0,
            "airline":"VY","flight_number":"1823",
            "departure_at":")JSON" + dep_30 + R"JSON(T11:00:00Z","return_at":")JSON" + ret_33 + R"JSON(T21:00:00Z"},
    "XXX": {"origin":"FRA","price":75.0,"transfers":0,
            "airline":"??","flight_number":"1",
            "departure_at":")JSON" + dep_30 + R"JSON(T11:00:00Z","return_at":")JSON" + ret_33 + R"JSON(T21:00:00Z"}
  },
  "currency": "eur",
  "error": null
})JSON";
}

ParseResult parse_flight_offers(const std::string& payload, const Filters& filters) {
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

    if (!root.is_object()) {
        result.problems.emplace_back("response is not a JSON object");
        return result;
    }

    // The API reports its own failures in the body with a 200 status, so a
    // successful HTTP request is not the same as a successful query.
    if (root.contains("error") && root.at("error").is_string() &&
        !root.at("error").get<std::string>().empty()) {
        result.problems.emplace_back("API error: " + root.at("error").get<std::string>());
        return result;
    }

    if (!root.contains("data") || !root.at("data").is_object()) {
        result.problems.emplace_back("response has no 'data' object");
        return result;
    }

    // The API echoes the currency lowercase ("eur"); ISO-4217 is uppercase and
    // that is what ends up in alerts.
    std::string currency = string_or(root, "currency", "EUR");
    for (char& c : currency) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // `data` is keyed by destination IATA, so iterate items rather than values:
    // the key is the authoritative destination even if the nested object omits
    // it.
    for (const auto& item : root.at("data").items()) {
        const std::string& key   = item.key();
        const json&        entry = item.value();

        if (!entry.is_object()) {
            result.problems.emplace_back(key + ": not an object");
            continue;
        }

        FlightOffer offer;
        offer.origin      = string_or(entry, "origin", "");
        offer.destination = string_or(entry, "destination", key);
        offer.currency    = currency;

        if (offer.origin.empty() || offer.destination.empty()) {
            result.problems.emplace_back(key + ": missing origin or destination");
            continue;
        }

        // Anywhere outside Europe or Turkey is not an error, just not ours.
        if (!geo::in_region(offer.destination)) {
            ++result.out_of_region;
            continue;
        }

        offer.departure_date = date_part(string_or(entry, "departure_at", ""));
        offer.return_date    = date_part(string_or(entry, "return_at", ""));

        if (offer.departure_date.empty()) {
            result.problems.emplace_back(key + ": no usable departure date");
            continue;
        }

        // ISO dates compare correctly as strings, which is the whole point of
        // the format.
        if ((!filters.earliest_departure.empty() &&
             offer.departure_date < filters.earliest_departure) ||
            (!filters.latest_departure.empty() &&
             offer.departure_date > filters.latest_departure)) {
            ++result.out_of_window;
            continue;
        }

        if (filters.min_nights > 0 || filters.max_nights > 0) {
            const int nights = offer.nights();
            if ((filters.min_nights > 0 && nights < filters.min_nights) ||
                (filters.max_nights > 0 && nights > filters.max_nights)) {
                ++result.wrong_length;
                continue;
            }
        }

        if (!entry.contains("price")) {
            result.problems.emplace_back(key + ": no price");
            continue;
        }

        const std::optional<double> amount = to_price(entry.at("price"));
        if (!amount.has_value()) {
            result.problems.emplace_back(key + ": price is not a number");
            continue;
        }

        // A zero or negative fare is a data bug, not a bargain worth alerting on.
        if (*amount <= 0.0) {
            result.problems.emplace_back(key + ": non-positive price");
            continue;
        }

        offer.price = *amount;
        result.offers.push_back(std::move(offer));
    }

    return result;
}

}  // namespace api
