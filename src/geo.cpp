#include "geo.h"

#include <algorithm>
#include <set>
#include <unordered_set>

namespace geo {
namespace {

// Country names exactly as the fare API spells them. Where the API's spelling
// is unusual ("Bosnia & Herzegovina", "Czechia"), both forms are listed rather
// than betting on one.
//
// An unrecognised country is treated as outside the region, which is the safe
// direction to fail: a missed destination, never a wrong alert. note_unknown()
// makes those misses visible.
const std::unordered_set<std::string>& allowlist() {
    static const std::unordered_set<std::string> countries = {
        "Albania",
        "Austria",
        "Belgium",
        "Bosnia & Herzegovina", "Bosnia and Herzegovina",
        "Bulgaria",
        "Croatia",
        "Cyprus",
        "Czechia", "Czech Republic",
        "Denmark",
        "Estonia",
        "Finland",
        "France",
        "Germany",
        "Greece",
        "Hungary",
        "Iceland",
        "Ireland",
        "Italy",
        "Kosovo",
        "Latvia",
        "Lithuania",
        "Luxembourg",
        "Malta",
        "Moldova",
        "Montenegro",
        "Netherlands", "The Netherlands",
        "North Macedonia", "Macedonia",
        "Norway",
        "Poland",
        "Portugal",
        "Romania",
        "Serbia",
        "Slovakia",
        "Slovenia",
        "Spain",
        "Sweden",
        "Switzerland",
        "United Kingdom", "UK",
    };
    return countries;
}

std::set<std::string>& unknown_store() {
    static std::set<std::string> seen;
    return seen;
}

}  // namespace

bool in_region(const std::string& country) {
    return allowlist().count(country) > 0;
}

void note_unknown(const std::string& country) {
    if (!country.empty() && !in_region(country)) unknown_store().insert(country);
}

std::vector<std::string> unknown_countries() {
    const auto& seen = unknown_store();
    return std::vector<std::string>(seen.begin(), seen.end());
}

std::size_t size() {
    return allowlist().size();
}

}  // namespace geo
