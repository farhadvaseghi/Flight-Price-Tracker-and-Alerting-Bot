#pragma once

#include <string>
#include <vector>

#include "flight.h"

namespace api {

// Amadeus Self-Service credentials. Empty means "run against the mock".
struct Credentials {
    std::string client_id;
    std::string client_secret;
    // The free tier lives on test.api.amadeus.com; production is a different
    // host with the same shape, so it is a setting rather than a constant.
    std::string host = "test.api.amadeus.com";

    bool present() const { return !client_id.empty() && !client_secret.empty(); }
};

// One Flight Inspiration Search query: everything reachable from one origin.
struct SearchParams {
    std::string origin;              // IATA of a German airport
    std::string departure_window;    // "YYYY-MM-DD,YYYY-MM-DD"
    int         min_nights = 2;
    int         max_nights = 4;
    double      max_price  = 0.0;    // 0 = let the API decide
    std::string currency   = "EUR";
};

// Outcome of parsing one response body.
//
// Parsing is deliberately partial: one broken entry in a batch must not cost
// us the rest, and must never take the process down. Usable offers land in
// `offers`, and everything rejected is described in `problems` so it can be
// logged. `filtered` counts destinations dropped for being outside the region,
// which is normal and not worth a log line each.
struct ParseResult {
    std::vector<FlightOffer> offers;
    std::vector<std::string> problems;
    int                      filtered = 0;
};

// Exchanges the credentials for a bearer token, caching it until it expires.
// Returns an empty string on failure.
std::string access_token(const Credentials& creds);

// Live call. Returns the raw response body, or "" on any failure -- the parser
// already treats an empty body as "nothing usable this cycle".
std::string fetch_flight_data(const Credentials& creds, const SearchParams& params);

// Hardcoded Amadeus-shaped payload, for testing without a key or network.
std::string mock_flight_data();

// Turns a Flight Inspiration Search body into offers, keeping only
// destinations inside Europe or Turkey.
ParseResult parse_flight_offers(const std::string& payload);

}  // namespace api
