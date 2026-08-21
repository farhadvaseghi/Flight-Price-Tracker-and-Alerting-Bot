#pragma once

#include <string>

// One flight offer as it comes back from the price API.
//
// This is the unit of currency between the layers: api_client builds these out
// of JSON, database stores them, notifier turns the interesting ones into
// Telegram messages.
//
// The API returns a matched outbound/return pair as a single priced offer, so
// departure_date and return_date belong to the same trip and `price` is the
// total for both legs.
struct FlightOffer {
    std::string origin;          // IATA code, e.g. "FRA"
    std::string destination;     // IATA code, e.g. "IST"
    std::string departure_date;  // ISO-8601 date, "YYYY-MM-DD"
    std::string return_date;     // empty for a one-way offer
    std::string currency;        // ISO-4217, e.g. "EUR"
    double      price = 0.0;     // total for the whole trip
    std::string booking_link;    // deep link supplied by the API, may be empty

    // Nights between the two legs, or 0 when one-way / dates unparseable.
    int nights() const;
};

// "YYYY-MM-DD" for today plus `offset_days`, in UTC.
std::string date_offset_utc(int offset_days);
