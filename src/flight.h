#pragma once

#include <string>

// One flight offer as it comes back from the price API.
//
// This is the unit of currency between the three layers: Step 3 builds these
// out of JSON, Step 2 stores them, Step 4 turns the interesting ones into
// Discord messages.
struct FlightOffer {
    std::string origin;          // IATA code, e.g. "FRA"
    std::string destination;     // IATA code, e.g. "TBS"
    std::string departure_date;  // ISO-8601 date, "YYYY-MM-DD"
    std::string airline;         // human-readable carrier name
    std::string currency;        // ISO-4217, e.g. "EUR"
    double      price = 0.0;
};
