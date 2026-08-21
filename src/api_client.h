#pragma once

#include <string>
#include <vector>

#include "flight.h"

namespace api {

// Returns the raw body of a flight-search response.
//
// For now this is a hardcoded payload so the pipeline can be exercised
// without an API key or network. Replacing it with a real call is a
// one-function change -- see the note in api_client.cpp.
std::string fetch_flight_data();

// Outcome of parsing one response body.
//
// Parsing is deliberately partial: one broken entry in a batch of twenty
// must not cost us the other nineteen, and must never take the process
// down. Whatever could be read lands in `offers`, and everything that was
// rejected is described in `problems` so it can be logged.
struct ParseResult {
    std::vector<FlightOffer> offers;
    std::vector<std::string> problems;
};

ParseResult parse_flight_offers(const std::string& payload);

}  // namespace api
