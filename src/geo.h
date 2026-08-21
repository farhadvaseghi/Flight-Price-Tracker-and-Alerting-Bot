#pragma once

#include <string>
#include <vector>

namespace geo {

// True when `country` is in the region we track.
//
// The fare API labels every airport with its country, so this is a country
// allowlist rather than a list of airports. That is both less to maintain and
// more accurate than guessing from IATA codes -- and it is what filters out the
// Moroccan and Jordanian destinations the API happily returns alongside the
// European ones.
//
// Turkey is deliberately absent: Ryanair has no Turkish routes, so including it
// would advertise coverage that cannot exist.
bool in_region(const std::string& country);

// Country names seen in responses that are not in the allowlist. Recorded so a
// missing entry shows up as a log line instead of silently dropping a
// destination forever.
void note_unknown(const std::string& country);
std::vector<std::string> unknown_countries();

// Number of countries in the allowlist, for diagnostics.
std::size_t size();

}  // namespace geo
