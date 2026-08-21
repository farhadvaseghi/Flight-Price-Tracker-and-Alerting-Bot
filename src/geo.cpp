#include "geo.h"

#include <unordered_map>

namespace geo {
namespace {

struct Entry {
    const char* iata;
    const char* city;
    const char* country;
    const char* code;
};

// Airports in Europe and Turkey with scheduled passenger traffic worth
// tracking. Not exhaustive by design: an airport missing here simply never
// produces an alert, which is a safe failure. Add rows as needed.
constexpr Entry kAirports[] = {
    // Turkey
    {"IST", "Istanbul",   "Turkey", "TR"}, {"SAW", "Istanbul",  "Turkey", "TR"},
    {"AYT", "Antalya",    "Turkey", "TR"}, {"ESB", "Ankara",    "Turkey", "TR"},
    {"ADB", "Izmir",      "Turkey", "TR"}, {"BJV", "Bodrum",    "Turkey", "TR"},
    {"DLM", "Dalaman",    "Turkey", "TR"}, {"ASR", "Kayseri",   "Turkey", "TR"},
    {"TZX", "Trabzon",    "Turkey", "TR"}, {"GZT", "Gaziantep", "Turkey", "TR"},
    {"ADA", "Adana",      "Turkey", "TR"}, {"NAV", "Nevsehir",  "Turkey", "TR"},

    // Germany (also valid as destinations)
    {"FRA", "Frankfurt",  "Germany", "DE"}, {"MUC", "Munich",    "Germany", "DE"},
    {"BER", "Berlin",     "Germany", "DE"}, {"DUS", "Dusseldorf","Germany", "DE"},
    {"HAM", "Hamburg",    "Germany", "DE"}, {"CGN", "Cologne",   "Germany", "DE"},
    {"STR", "Stuttgart",  "Germany", "DE"}, {"HAJ", "Hannover",  "Germany", "DE"},
    {"NUE", "Nuremberg",  "Germany", "DE"}, {"LEJ", "Leipzig",   "Germany", "DE"},
    {"DTM", "Dortmund",   "Germany", "DE"}, {"BRE", "Bremen",    "Germany", "DE"},
    {"FMM", "Memmingen",  "Germany", "DE"}, {"FKB", "Karlsruhe", "Germany", "DE"},
    {"FMO", "Munster",    "Germany", "DE"}, {"PAD", "Paderborn", "Germany", "DE"},
    {"SCN", "Saarbrucken","Germany", "DE"}, {"DRS", "Dresden",   "Germany", "DE"},

    // United Kingdom & Ireland
    {"LHR", "London",     "United Kingdom", "GB"}, {"LGW", "London",  "United Kingdom", "GB"},
    {"STN", "London",     "United Kingdom", "GB"}, {"LTN", "London",  "United Kingdom", "GB"},
    {"LCY", "London",     "United Kingdom", "GB"}, {"MAN", "Manchester", "United Kingdom", "GB"},
    {"EDI", "Edinburgh",  "United Kingdom", "GB"}, {"GLA", "Glasgow", "United Kingdom", "GB"},
    {"BHX", "Birmingham", "United Kingdom", "GB"}, {"BRS", "Bristol", "United Kingdom", "GB"},
    {"NCL", "Newcastle",  "United Kingdom", "GB"}, {"LPL", "Liverpool", "United Kingdom", "GB"},
    {"BFS", "Belfast",    "United Kingdom", "GB"}, {"LBA", "Leeds",   "United Kingdom", "GB"},
    {"DUB", "Dublin",     "Ireland", "IE"}, {"ORK", "Cork",     "Ireland", "IE"},
    {"SNN", "Shannon",    "Ireland", "IE"},

    // France
    {"CDG", "Paris",      "France", "FR"}, {"ORY", "Paris",     "France", "FR"},
    {"BVA", "Paris",      "France", "FR"}, {"NCE", "Nice",      "France", "FR"},
    {"LYS", "Lyon",       "France", "FR"}, {"MRS", "Marseille", "France", "FR"},
    {"TLS", "Toulouse",   "France", "FR"}, {"BOD", "Bordeaux",  "France", "FR"},
    {"NTE", "Nantes",     "France", "FR"}, {"LIL", "Lille",     "France", "FR"},
    {"SXB", "Strasbourg", "France", "FR"}, {"BIQ", "Biarritz",  "France", "FR"},
    {"AJA", "Ajaccio",    "France", "FR"},

    // Spain & Portugal
    {"MAD", "Madrid",     "Spain", "ES"}, {"BCN", "Barcelona",  "Spain", "ES"},
    {"AGP", "Malaga",     "Spain", "ES"}, {"PMI", "Palma",      "Spain", "ES"},
    {"ALC", "Alicante",   "Spain", "ES"}, {"VLC", "Valencia",   "Spain", "ES"},
    {"SVQ", "Seville",    "Spain", "ES"}, {"BIO", "Bilbao",     "Spain", "ES"},
    {"IBZ", "Ibiza",      "Spain", "ES"}, {"TFS", "Tenerife",   "Spain", "ES"},
    {"TFN", "Tenerife",   "Spain", "ES"}, {"LPA", "Las Palmas", "Spain", "ES"},
    {"ACE", "Lanzarote",  "Spain", "ES"}, {"FUE", "Fuerteventura", "Spain", "ES"},
    {"SCQ", "Santiago",   "Spain", "ES"}, {"GRX", "Granada",    "Spain", "ES"},
    {"LIS", "Lisbon",     "Portugal", "PT"}, {"OPO", "Porto",   "Portugal", "PT"},
    {"FAO", "Faro",       "Portugal", "PT"}, {"FNC", "Funchal",  "Portugal", "PT"},
    {"PDL", "Ponta Delgada", "Portugal", "PT"},

    // Italy, Malta
    {"FCO", "Rome",       "Italy", "IT"}, {"CIA", "Rome",       "Italy", "IT"},
    {"MXP", "Milan",      "Italy", "IT"}, {"LIN", "Milan",      "Italy", "IT"},
    {"BGY", "Milan",      "Italy", "IT"}, {"VCE", "Venice",     "Italy", "IT"},
    {"TSF", "Venice",     "Italy", "IT"}, {"NAP", "Naples",     "Italy", "IT"},
    {"BLQ", "Bologna",    "Italy", "IT"}, {"FLR", "Florence",   "Italy", "IT"},
    {"PSA", "Pisa",       "Italy", "IT"}, {"TRN", "Turin",      "Italy", "IT"},
    {"CTA", "Catania",    "Italy", "IT"}, {"PMO", "Palermo",    "Italy", "IT"},
    {"BRI", "Bari",       "Italy", "IT"}, {"CAG", "Cagliari",   "Italy", "IT"},
    {"OLB", "Olbia",      "Italy", "IT"}, {"VRN", "Verona",     "Italy", "IT"},
    {"MLA", "Malta",      "Malta", "MT"},

    // Benelux, Switzerland, Austria
    {"AMS", "Amsterdam",  "Netherlands", "NL"}, {"EIN", "Eindhoven", "Netherlands", "NL"},
    {"RTM", "Rotterdam",  "Netherlands", "NL"}, {"BRU", "Brussels",  "Belgium", "BE"},
    {"CRL", "Brussels",   "Belgium", "BE"}, {"ANR", "Antwerp",    "Belgium", "BE"},
    {"LUX", "Luxembourg", "Luxembourg", "LU"},
    {"ZRH", "Zurich",     "Switzerland", "CH"}, {"GVA", "Geneva",  "Switzerland", "CH"},
    {"BSL", "Basel",      "Switzerland", "CH"}, {"BRN", "Bern",    "Switzerland", "CH"},
    {"VIE", "Vienna",     "Austria", "AT"}, {"SZG", "Salzburg",  "Austria", "AT"},
    {"INN", "Innsbruck",  "Austria", "AT"}, {"GRZ", "Graz",      "Austria", "AT"},

    // Nordics & Baltics
    {"CPH", "Copenhagen", "Denmark", "DK"}, {"BLL", "Billund",   "Denmark", "DK"},
    {"AAL", "Aalborg",    "Denmark", "DK"},
    {"ARN", "Stockholm",  "Sweden", "SE"}, {"NYO", "Stockholm",  "Sweden", "SE"},
    {"GOT", "Gothenburg", "Sweden", "SE"}, {"MMX", "Malmo",      "Sweden", "SE"},
    {"OSL", "Oslo",       "Norway", "NO"}, {"TRF", "Oslo",       "Norway", "NO"},
    {"BGO", "Bergen",     "Norway", "NO"}, {"TRD", "Trondheim",  "Norway", "NO"},
    {"SVG", "Stavanger",  "Norway", "NO"}, {"TOS", "Tromso",     "Norway", "NO"},
    {"HEL", "Helsinki",   "Finland", "FI"}, {"TMP", "Tampere",   "Finland", "FI"},
    {"OUL", "Oulu",       "Finland", "FI"}, {"RVN", "Rovaniemi", "Finland", "FI"},
    {"KEF", "Reykjavik",  "Iceland", "IS"},
    {"TLL", "Tallinn",    "Estonia", "EE"}, {"RIX", "Riga",      "Latvia", "LV"},
    {"VNO", "Vilnius",    "Lithuania", "LT"}, {"KUN", "Kaunas",  "Lithuania", "LT"},

    // Central & Eastern Europe
    {"PRG", "Prague",     "Czechia", "CZ"}, {"BRQ", "Brno",      "Czechia", "CZ"},
    {"WAW", "Warsaw",     "Poland", "PL"}, {"WMI", "Warsaw",     "Poland", "PL"},
    {"KRK", "Krakow",     "Poland", "PL"}, {"GDN", "Gdansk",     "Poland", "PL"},
    {"WRO", "Wroclaw",    "Poland", "PL"}, {"POZ", "Poznan",     "Poland", "PL"},
    {"KTW", "Katowice",   "Poland", "PL"},
    {"BUD", "Budapest",   "Hungary", "HU"}, {"BTS", "Bratislava", "Slovakia", "SK"},
    {"KSC", "Kosice",     "Slovakia", "SK"},
    {"OTP", "Bucharest",  "Romania", "RO"}, {"CLJ", "Cluj",      "Romania", "RO"},
    {"TSR", "Timisoara",  "Romania", "RO"}, {"IAS", "Iasi",      "Romania", "RO"},
    {"SOF", "Sofia",      "Bulgaria", "BG"}, {"VAR", "Varna",    "Bulgaria", "BG"},
    {"BOJ", "Burgas",     "Bulgaria", "BG"},

    // Balkans & Greece & Cyprus
    {"ATH", "Athens",     "Greece", "GR"}, {"SKG", "Thessaloniki", "Greece", "GR"},
    {"HER", "Heraklion",  "Greece", "GR"}, {"RHO", "Rhodes",     "Greece", "GR"},
    {"CHQ", "Chania",     "Greece", "GR"}, {"CFU", "Corfu",      "Greece", "GR"},
    {"JMK", "Mykonos",    "Greece", "GR"}, {"JTR", "Santorini",  "Greece", "GR"},
    {"KGS", "Kos",        "Greece", "GR"}, {"ZTH", "Zakynthos",  "Greece", "GR"},
    {"ZAG", "Zagreb",     "Croatia", "HR"}, {"SPU", "Split",     "Croatia", "HR"},
    {"DBV", "Dubrovnik",  "Croatia", "HR"}, {"PUY", "Pula",      "Croatia", "HR"},
    {"ZAD", "Zadar",      "Croatia", "HR"},
    {"LJU", "Ljubljana",  "Slovenia", "SI"}, {"BEG", "Belgrade", "Serbia", "RS"},
    {"INI", "Nis",        "Serbia", "RS"},
    {"SJJ", "Sarajevo",   "Bosnia and Herzegovina", "BA"},
    {"TIA", "Tirana",     "Albania", "AL"}, {"SKP", "Skopje",    "North Macedonia", "MK"},
    {"TGD", "Podgorica",  "Montenegro", "ME"}, {"TIV", "Tivat",  "Montenegro", "ME"},
    {"PRN", "Pristina",   "Kosovo", "XK"},
    {"LCA", "Larnaca",    "Cyprus", "CY"}, {"PFO", "Paphos",     "Cyprus", "CY"},
};

const std::unordered_map<std::string, Place>& table() {
    // Built once on first use; ~180 entries, so the cost is negligible and it
    // avoids a static initialisation order problem.
    static const std::unordered_map<std::string, Place> map = [] {
        std::unordered_map<std::string, Place> m;
        m.reserve(sizeof(kAirports) / sizeof(kAirports[0]) * 2);
        for (const Entry& e : kAirports) {
            m.emplace(e.iata, Place{e.city, e.country, e.code});
        }
        return m;
    }();
    return map;
}

}  // namespace

bool in_region(const std::string& iata) {
    return table().count(iata) > 0;
}

Place lookup(const std::string& iata) {
    const auto it = table().find(iata);
    if (it != table().end()) return it->second;
    return Place{iata, "", ""};
}

std::size_t size() {
    return table().size();
}

}  // namespace geo
