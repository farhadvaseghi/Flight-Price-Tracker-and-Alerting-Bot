#include "flight.h"

#include <cstdlib>

namespace {

// Howard Hinnant's days_from_civil. Converts a proleptic Gregorian date to a
// day number, which makes the difference between two dates plain subtraction.
// std::chrono's calendar types would do this natively, but they need C++20 and
// this project targets C++17.
long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned  yoe = static_cast<unsigned>(y - era * 400);            // [0, 399]
    const unsigned  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// Parses exactly "YYYY-MM-DD"; anything else is rejected rather than guessed.
bool parse_date(const std::string& text, int& y, unsigned& m, unsigned& d) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (text[i] < '0' || text[i] > '9') return false;
    }
    y = std::atoi(text.substr(0, 4).c_str());
    m = static_cast<unsigned>(std::atoi(text.substr(5, 2).c_str()));
    d = static_cast<unsigned>(std::atoi(text.substr(8, 2).c_str()));
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

}  // namespace

int FlightOffer::nights() const {
    if (return_date.empty()) return 0;

    int      dy = 0, ry = 0;
    unsigned dm = 0, dd = 0, rm = 0, rd = 0;
    if (!parse_date(departure_date, dy, dm, dd)) return 0;
    if (!parse_date(return_date, ry, rm, rd))    return 0;

    const long long diff = days_from_civil(ry, rm, rd) - days_from_civil(dy, dm, dd);
    return diff > 0 ? static_cast<int>(diff) : 0;
}
