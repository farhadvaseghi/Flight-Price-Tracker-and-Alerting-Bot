#pragma once

#include <string>
#include <vector>

#include "flight.h"

namespace api {

// Where fares come from.
//
// Ryanair's fare finder is the JSON endpoint their own site calls. It needs no
// key, no account and no signup, which is what makes this whole project free to
// run. The flip side is that it is undocumented: it can change without notice,
// and it only knows about Ryanair's own network.
struct Settings {
    std::string host = "services-api.ryanair.com";
    bool        use_mock = false;   // serve the built-in payload instead
};

// One page of one origin's fares.
//
// Unlike the providers this replaced, the endpoint filters by date window and
// trip length server-side, so those constraints travel with the request rather
// than being applied afterwards.
struct SearchParams {
    std::string origin;              // IATA of a German airport
    std::string earliest_departure;  // "YYYY-MM-DD"
    std::string latest_departure;
    int         min_nights = 2;
    int         max_nights = 4;
    std::string currency   = "EUR";
    int         offset     = 0;      // paging: 0, 20, 40, ...
};

// The endpoint rejects any limit above this, so paging is not optional.
constexpr int kPageSize = 20;

// Outcome of parsing one page.
//
// Parsing is deliberately partial: one broken entry must not cost us the rest,
// and must never take the process down. Usable offers land in `offers`, entries
// that were malformed are described in `problems`, and entries that were merely
// outside the region are counted rather than logged.
//
// Note what is absent: a price filter. The cap is an alerting decision, not a
// fetching one -- dropping expensive fares before the store would leave a route
// that normally sits at 200 EUR with no history, so the day it fell to 95 it
// would look like a first sighting and stay silent.
struct ParseResult {
    std::vector<FlightOffer> offers;
    std::vector<std::string> problems;
    int  out_of_region = 0;
    bool has_more      = false;   // another page is available
};

// Fetches one page. Returns the raw body, or "" on any failure -- the parser
// already treats an empty body as "nothing usable".
std::string fetch_flight_data(const Settings& settings, const SearchParams& params);

// A hardcoded response in the real endpoint's shape, for testing offline.
// Dates are generated relative to today so the payload cannot go stale.
std::string mock_flight_data();

ParseResult parse_flight_offers(const std::string& payload);

// Deep link to the booking page for one offer, or "" when the dates are
// incomplete. Exposed so --test-alert builds its sample the same way a real
// offer is built, rather than carrying a second copy of the URL format that
// could drift out of step with this one.
std::string booking_url(const FlightOffer& offer);

}  // namespace api
