#pragma once

#include <string>

namespace geo {

struct Place {
    std::string city;          // "Istanbul"
    std::string country;       // "Turkey"
    std::string country_code;  // ISO 3166-1 alpha-2, "TR"
};

// True when `iata` is an airport in Europe or Turkey that we care about.
//
// This is an allowlist, deliberately. Amadeus returns destinations worldwide,
// and resolving each one's country through the reference-data API would spend
// quota on every new airport. A bundled table costs nothing, works offline,
// and treats anything unknown as "not our region" -- which conveniently also
// keeps obscure airfields out of the alerts.
bool in_region(const std::string& iata);

// Human-readable place for an allowlisted IATA code. Returns the code itself
// as the city when unknown, so a message never renders as an empty string.
Place lookup(const std::string& iata);

// Number of airports in the table, for diagnostics.
std::size_t size();

}  // namespace geo
