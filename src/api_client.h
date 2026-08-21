#pragma once

#include <string>
#include <vector>

#include "flight.h"

namespace api {

// Travelpayouts Data API credentials.
//
// A single affiliate token, passed as a query parameter -- there is no OAuth
// step. It is still a credential: it identifies your account and is rate
// limited per token, so it belongs in the environment, never committed.
//
// Empty means "run against the built-in mock".
struct Credentials {
    std::string token;
    std::string host = "api.travelpayouts.com";

    bool present() const { return !token.empty(); }
};

// One city-directions query: everything cheap reachable from one origin.
struct SearchParams {
    std::string origin;             // IATA of a German airport
    std::string currency = "eur";
};

// Client-side filters.
//
// The API returns whatever cheap fares it has cached from this origin, with no
// way to constrain dates, trip length or price in the request. So the narrowing
// happens here instead, and each reason is counted separately -- a sweep that
// suddenly returns nothing should say whether that is the region filter, the
// date window or the price cap.
struct Filters {
    std::string earliest_departure;  // "YYYY-MM-DD", empty = no bound
    std::string latest_departure;
    int         min_nights = 0;      // 0 = no bound
    int         max_nights = 0;
};

// Note what is deliberately absent: a price filter.
//
// The price cap is an alerting decision, not a fetching one. Dropping
// expensive fares here would keep them out of the store, so a route that
// normally sits at 200 EUR would have no history, and the day it fell to 95
// would look like a first sighting rather than a record low -- and stay
// silent. Everything in region and in window is recorded; main.cpp decides
// what is worth waking someone for.

// Outcome of parsing one response body.
//
// Parsing is deliberately partial: one broken entry must not cost us the rest,
// and must never take the process down. Usable offers land in `offers`, entries
// that were malformed are described in `problems`, and entries that were merely
// uninteresting are counted rather than logged.
struct ParseResult {
    std::vector<FlightOffer> offers;
    std::vector<std::string> problems;
    int out_of_region = 0;
    int out_of_window = 0;   // departure outside the date range
    int wrong_length  = 0;   // trip too short or too long

    int rejected() const { return out_of_region + out_of_window + wrong_length; }
};

// Live call. Returns the raw response body, or "" on any failure -- the parser
// already treats an empty body as "nothing usable this cycle".
std::string fetch_flight_data(const Credentials& creds, const SearchParams& params);

// Hardcoded response in the real API's shape, for testing without a token or
// network. Dates are generated relative to today so the payload does not go
// stale and silently start failing every filter.
std::string mock_flight_data();

ParseResult parse_flight_offers(const std::string& payload, const Filters& filters);

}  // namespace api
