#include "api_client.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

#include "geo.h"

namespace api {
namespace {

using nlohmann::json;

constexpr int kSearchTimeoutMs = 25000;

// Fetched data is never trusted to have the shape we expect, so every read
// below goes through one of these helpers rather than operator[] or at().

const json& child(const json& node, const char* key) {
    static const json kEmpty = json::object();
    if (!node.is_object() || !node.contains(key)) return kEmpty;
    return node.at(key);
}

// value() would throw type_error.302 if the key existed but held null, which is
// exactly what this API does for fields it has no data for. Check the type.
std::string string_or(const json& node, const char* key, const std::string& fallback) {
    if (!node.is_object() || !node.contains(key)) return fallback;
    const json& value = node.at(key);
    return value.is_string() ? value.get<std::string>() : fallback;
}

std::optional<double> number_at(const json& node, const char* key) {
    if (!node.is_object() || !node.contains(key)) return std::nullopt;
    const json& value = node.at(key);
    if (value.is_number()) return value.get<double>();

    // Some providers quote money as a decimal string, so accept that too.
    if (value.is_string()) {
        try {
            const std::string text     = value.get<std::string>();
            std::size_t       consumed = 0;
            const double      parsed   = std::stod(text, &consumed);
            if (consumed != text.size()) return std::nullopt;
            return parsed;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// departureDate is a local timestamp ("2026-09-28T22:00:00"), but the routes
// table works in whole days. Take the date part, and only if it looks like one.
std::string date_part(const std::string& timestamp) {
    if (timestamp.size() < 10) return "";
    const std::string date = timestamp.substr(0, 10);
    if (date[4] != '-' || date[7] != '-') return "";
    return date;
}

// Ryanair's booking page accepts the trip as query parameters, so an alert can
// link straight at the fare rather than making you search for it again.
std::string booking_url(const FlightOffer& offer) {
    if (offer.departure_date.empty() || offer.return_date.empty()) return "";
    return "https://www.ryanair.com/gb/en/trip/flights/select"
           "?adults=1&teens=0&children=0&infants=0"
           "&dateOut=" + offer.departure_date +
           "&dateIn="  + offer.return_date +
           "&originIata=" + offer.origin +
           "&destinationIata=" + offer.destination +
           "&isReturn=true";
}

}  // namespace

std::string fetch_flight_data(const Settings& settings, const SearchParams& params) {
    if (settings.use_mock) return mock_flight_data();

    const cpr::Response response = cpr::Get(
        cpr::Url{"https://" + settings.host + "/farfnd/v4/roundTripFares"},
        cpr::Parameters{
            {"departureAirportIataCode", params.origin},
            {"outboundDepartureDateFrom", params.earliest_departure},
            {"outboundDepartureDateTo",   params.latest_departure},
            {"inboundDepartureDateFrom",  params.earliest_departure},
            {"inboundDepartureDateTo",    params.latest_departure},
            {"durationFrom", std::to_string(params.min_nights)},
            {"durationTo",   std::to_string(params.max_nights)},
            {"currency", params.currency},
            {"limit",  std::to_string(kPageSize)},
            {"offset", std::to_string(params.offset)},
        },
        // A default libcurl user agent gets an empty result set back.
        cpr::Header{{"User-Agent", "Mozilla/5.0"},
                    {"Accept", "application/json"},
                    {"Accept-Encoding", "gzip, deflate"}},
        cpr::Timeout{kSearchTimeoutMs});

    if (response.error || response.status_code != 200) return "";
    return response.text;
}

std::string mock_flight_data() {
    // Shaped exactly like a roundTripFares page. Dates are relative to today
    // rather than hardcoded: fixed dates would drift out of the search window
    // as time passed, and the mock would silently stop matching, taking the
    // tests with it.
    //
    // Entries: two usable (Malaga cheap, Athens above the cap), one outside the
    // region, one with no price, and one with no arrival airport.
    const std::string out1 = date_offset_utc(30), in1 = date_offset_utc(33);
    const std::string out2 = date_offset_utc(45), in2 = date_offset_utc(48);

    auto airport = [](const std::string& country, const std::string& iata,
                      const std::string& name) {
        return R"({"countryName":")" + country + R"(","iataCode":")" + iata +
               R"(","name":")" + name + R"(","city":{"name":")" + name + R"("}})";
    };

    const std::string cgn = airport("Germany", "CGN", "Cologne");
    const std::string agp = airport("Spain", "AGP", "Malaga");
    const std::string ath = airport("Greece", "ATH", "Athens");
    const std::string aga = airport("Morocco", "AGA", "Agadir");
    const std::string blq = airport("Italy", "BLQ", "Bologna");

    auto leg = [](const std::string& from, const std::string& to,
                  const std::string& date, const std::string& flight) {
        return R"({"departureAirport":)" + from + R"(,"arrivalAirport":)" + to +
               R"(,"departureDate":")" + date + R"(","flightNumber":")" + flight + R"("})";
    };

    return std::string(R"({"arrivalAirportCategories":null,"fares":[)") +

        R"({"outbound":)" + leg(cgn, agp, out1 + "T22:00:00", "FR2308") +
        R"(,"inbound":)"  + leg(agp, cgn, in1 + "T21:20:00", "FR2307") +
        R"(,"summary":{"price":{"value":88.99,"currencyCode":"EUR"},"tripDurationDays":3}},)" +

        R"({"outbound":)" + leg(cgn, ath, out2 + "T06:00:00", "FR8801") +
        R"(,"inbound":)"  + leg(ath, cgn, in2 + "T19:30:00", "FR8802") +
        R"(,"summary":{"price":{"value":142.30,"currencyCode":"EUR"},"tripDurationDays":3}},)" +

        R"({"outbound":)" + leg(cgn, aga, out1 + "T09:00:00", "FR1234") +
        R"(,"inbound":)"  + leg(aga, cgn, in1 + "T18:00:00", "FR1235") +
        R"(,"summary":{"price":{"value":95.00,"currencyCode":"EUR"},"tripDurationDays":3}},)" +

        R"({"outbound":)" + leg(cgn, blq, out1 + "T11:00:00", "FR555") +
        R"(,"inbound":)"  + leg(blq, cgn, in1 + "T21:00:00", "FR556") +
        R"(,"summary":{"tripDurationDays":3}},)" +

        R"({"outbound":{"departureAirport":)" + cgn +
        R"(,"departureDate":")" + out1 + R"(T11:00:00","flightNumber":"FR777"})" +
        R"(,"inbound":{"departureDate":")" + in1 + R"(T21:00:00"})" +
        R"(,"summary":{"price":{"value":75.00,"currencyCode":"EUR"},"tripDurationDays":3}})" +

        R"(],"nextPage":null,"size":5})";
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

    if (!root.is_object()) {
        result.problems.emplace_back("response is not a JSON object");
        return result;
    }

    // The endpoint reports its own rejections in the body: an out-of-range
    // limit comes back as {"code":"InvalidLimit"} with a 200 status, so a
    // successful request is not the same as a successful query.
    if (root.contains("code") && root.at("code").is_string()) {
        result.problems.emplace_back("API rejected the request: " +
                                     root.at("code").get<std::string>());
        return result;
    }

    if (!root.contains("fares") || !root.at("fares").is_array()) {
        result.problems.emplace_back("response has no 'fares' array");
        return result;
    }

    // nextPage holds a page number, or null on the last page.
    result.has_more = root.contains("nextPage") && !root.at("nextPage").is_null();

    std::size_t index = 0;
    for (const json& fare : root.at("fares")) {
        const std::string label = "fare[" + std::to_string(index++) + "]";

        if (!fare.is_object()) {
            result.problems.emplace_back(label + ": not an object");
            continue;
        }

        const json& outbound = child(fare, "outbound");
        const json& inbound  = child(fare, "inbound");
        const json& from     = child(outbound, "departureAirport");
        const json& to       = child(outbound, "arrivalAirport");

        FlightOffer offer;
        offer.origin              = string_or(from, "iataCode", "");
        offer.destination         = string_or(to, "iataCode", "");
        offer.origin_city         = string_or(child(from, "city"), "name",
                                              string_or(from, "name", offer.origin));
        offer.destination_city    = string_or(child(to, "city"), "name",
                                              string_or(to, "name", offer.destination));
        offer.destination_country = string_or(to, "countryName", "");

        // Origin and destination form the primary key of the routes table, so
        // an entry missing either cannot be stored at all.
        if (offer.origin.empty() || offer.destination.empty()) {
            result.problems.emplace_back(label + ": missing origin or destination");
            continue;
        }

        // Outside the tracked region is not an error, just not ours. Record the
        // country so a gap in the allowlist surfaces instead of hiding.
        if (!geo::in_region(offer.destination_country)) {
            geo::note_unknown(offer.destination_country);
            ++result.out_of_region;
            continue;
        }

        offer.departure_date = date_part(string_or(outbound, "departureDate", ""));
        offer.return_date    = date_part(string_or(inbound, "departureDate", ""));

        if (offer.departure_date.empty()) {
            result.problems.emplace_back(label + " " + offer.destination +
                                         ": no usable departure date");
            continue;
        }

        // The trip price is the summary total, not either leg's own fare.
        const json&                 summary = child(fare, "summary");
        const json&                 price   = child(summary, "price");
        const std::optional<double> amount  = number_at(price, "value");

        if (!amount.has_value()) {
            result.problems.emplace_back(label + " " + offer.destination + ": no price");
            continue;
        }

        // A zero or negative fare is a data bug, not a bargain worth alerting on.
        if (*amount <= 0.0) {
            result.problems.emplace_back(label + " " + offer.destination +
                                         ": non-positive price");
            continue;
        }

        offer.price        = *amount;
        offer.currency     = string_or(price, "currencyCode", "EUR");
        offer.booking_link = booking_url(offer);

        result.offers.push_back(std::move(offer));
    }

    return result;
}

}  // namespace api
