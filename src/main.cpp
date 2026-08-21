// Step 1 smoke test: proves cpr, nlohmann/json and sqlite3 all link correctly.
// This file gets replaced by the real application loop in Step 5.

#include <iostream>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

int main() {
    std::cout << "flight_tracker - dependency smoke test\n";

    std::cout << "  sqlite3 : " << sqlite3_libversion() << '\n';
    std::cout << "  json    : " << NLOHMANN_JSON_VERSION_MAJOR << '.'
                                << NLOHMANN_JSON_VERSION_MINOR << '.'
                                << NLOHMANN_JSON_VERSION_PATCH << '\n';

    // A real HTTPS round-trip, which also validates that TLS works.
    cpr::Response r = cpr::Get(cpr::Url{"https://httpbin.org/get"},
                               cpr::Timeout{5000});
    std::cout << "  cpr     : HTTP " << r.status_code
              << (r.error ? (" (" + r.error.message + ")") : "") << '\n';

    return 0;
}
