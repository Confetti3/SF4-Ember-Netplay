#pragma once
#include "Area.hxx"
#include "Country.hxx"
#include <string>

// The area Automatic stands for, from what Windows says: its time zone (the
// key name of GetDynamicTimeZoneInformation, such as "Pacific Standard Time"),
// with the country of its Region setting as the tie-break. Pure, over the two
// strings, so every zone and country is tested without Windows (area_test.cxx).
// Under Wine or Proton the zone follows the host's clock, which is still the
// player's own. Nothing here asks the network.
namespace sf4e { namespace areas {

// A Windows time zone and the areas it covers: the first is the zone's own,
// the others areas its places also lie in, which the country picks between.
// A zone that names no place of its own (UTC, the date line) has none, and the
// country decides.
struct Zone { const char* key; Area areas[3]; };
constexpr Zone Zones[] = {
	{"Dateline Standard Time", {}},
	{"UTC-11", {Area::Oceania}},
	{"Aleutian Standard Time", {Area::NaWest}},
	{"Hawaiian Standard Time", {Area::NaWest, Area::Oceania}},
	{"Marquesas Standard Time", {Area::Oceania}},
	{"Alaskan Standard Time", {Area::NaWest}},
	{"UTC-09", {Area::Oceania}},
	{"Pacific Standard Time (Mexico)", {Area::NaWest}},
	{"UTC-08", {Area::Oceania}},
	{"Pacific Standard Time", {Area::NaWest}},
	{"US Mountain Standard Time", {Area::NaWest}},
	{"Mountain Standard Time (Mexico)", {Area::NaWest}},
	{"Mountain Standard Time", {Area::NaWest}},
	{"Yukon Standard Time", {Area::NaWest}},
	{"Central America Standard Time", {Area::CentralAmerica, Area::SouthAmerica}},
	{"Central Standard Time", {Area::NaCentral}},
	{"Easter Island Standard Time", {Area::SouthAmerica}},
	{"Central Standard Time (Mexico)", {Area::NaCentral}},
	{"Canada Central Standard Time", {Area::NaCentral}},
	{"SA Pacific Standard Time", {Area::SouthAmerica, Area::CentralAmerica}},
	{"Eastern Standard Time (Mexico)", {Area::NaCentral}},
	{"Eastern Standard Time", {Area::NaEast, Area::CentralAmerica}},
	{"Haiti Standard Time", {Area::CentralAmerica}},
	{"Cuba Standard Time", {Area::CentralAmerica}},
	{"US Eastern Standard Time", {Area::NaEast}},
	{"Turks And Caicos Standard Time", {Area::CentralAmerica}},
	{"Paraguay Standard Time", {Area::SouthAmerica}},
	{"Atlantic Standard Time", {Area::NaEast}},
	{"Venezuela Standard Time", {Area::SouthAmerica}},
	{"Central Brazilian Standard Time", {Area::SouthAmerica}},
	{"SA Western Standard Time", {Area::SouthAmerica, Area::CentralAmerica}},
	{"Pacific SA Standard Time", {Area::SouthAmerica}},
	{"Newfoundland Standard Time", {Area::NaEast}},
	{"Tocantins Standard Time", {Area::SouthAmerica}},
	{"E. South America Standard Time", {Area::SouthAmerica}},
	{"SA Eastern Standard Time", {Area::SouthAmerica}},
	{"Argentina Standard Time", {Area::SouthAmerica}},
	{"Greenland Standard Time", {Area::NaEast}},
	{"Montevideo Standard Time", {Area::SouthAmerica}},
	{"Magallanes Standard Time", {Area::SouthAmerica}},
	{"Saint Pierre Standard Time", {Area::NaEast}},
	{"Bahia Standard Time", {Area::SouthAmerica}},
	{"UTC-02", {Area::SouthAmerica}},
	{"Mid-Atlantic Standard Time", {}},
	{"Azores Standard Time", {Area::EuropeWest}},
	{"Cape Verde Standard Time", {Area::MiddleEastAfrica}},
	{"UTC", {}},
	{"GMT Standard Time", {Area::EuropeWest}},
	{"Greenwich Standard Time", {Area::MiddleEastAfrica, Area::EuropeWest}},
	{"Sao Tome Standard Time", {Area::MiddleEastAfrica}},
	{"Morocco Standard Time", {Area::MiddleEastAfrica}},
	{"W. Europe Standard Time", {Area::EuropeWest}},
	{"Central Europe Standard Time", {Area::EuropeEast}},
	{"Romance Standard Time", {Area::EuropeWest}},
	{"Central European Standard Time", {Area::EuropeEast}},
	{"W. Central Africa Standard Time", {Area::MiddleEastAfrica}},
	{"Jordan Standard Time", {Area::MiddleEastAfrica}},
	{"GTB Standard Time", {Area::EuropeEast}},
	{"Middle East Standard Time", {Area::MiddleEastAfrica}},
	{"Egypt Standard Time", {Area::MiddleEastAfrica}},
	{"E. Europe Standard Time", {Area::EuropeEast}},
	{"Syria Standard Time", {Area::MiddleEastAfrica}},
	{"West Bank Standard Time", {Area::MiddleEastAfrica}},
	{"South Africa Standard Time", {Area::MiddleEastAfrica}},
	{"FLE Standard Time", {Area::EuropeEast}},
	{"Israel Standard Time", {Area::MiddleEastAfrica}},
	{"South Sudan Standard Time", {Area::MiddleEastAfrica}},
	{"Kaliningrad Standard Time", {Area::EuropeEast}},
	{"Sudan Standard Time", {Area::MiddleEastAfrica}},
	{"Libya Standard Time", {Area::MiddleEastAfrica}},
	{"Namibia Standard Time", {Area::MiddleEastAfrica}},
	{"Arabic Standard Time", {Area::MiddleEastAfrica}},
	{"Turkey Standard Time", {Area::EuropeEast}},
	{"Arab Standard Time", {Area::MiddleEastAfrica}},
	{"Belarus Standard Time", {Area::EuropeEast}},
	{"Russian Standard Time", {Area::EuropeEast}},
	{"E. Africa Standard Time", {Area::MiddleEastAfrica}},
	{"Volgograd Standard Time", {Area::EuropeEast}},
	{"Iran Standard Time", {Area::MiddleEastAfrica}},
	{"Arabian Standard Time", {Area::MiddleEastAfrica}},
	{"Astrakhan Standard Time", {Area::EuropeEast}},
	{"Azerbaijan Standard Time", {Area::EuropeEast}},
	{"Russia Time Zone 3", {Area::EuropeEast}},
	{"Mauritius Standard Time", {Area::MiddleEastAfrica}},
	{"Saratov Standard Time", {Area::EuropeEast}},
	{"Georgian Standard Time", {Area::EuropeEast}},
	{"Caucasus Standard Time", {Area::EuropeEast}},
	{"Afghanistan Standard Time", {Area::SouthAsia}},
	{"West Asia Standard Time", {Area::EuropeEast, Area::SouthAsia}},
	{"Ekaterinburg Standard Time", {Area::EuropeEast}},
	{"Pakistan Standard Time", {Area::SouthAsia}},
	{"Qyzylorda Standard Time", {Area::EuropeEast}},
	{"India Standard Time", {Area::SouthAsia}},
	{"Sri Lanka Standard Time", {Area::SouthAsia}},
	{"Nepal Standard Time", {Area::SouthAsia}},
	{"Central Asia Standard Time", {Area::EuropeEast, Area::EastAsia, Area::SouthAsia}},
	{"Bangladesh Standard Time", {Area::SouthAsia}},
	{"Omsk Standard Time", {Area::EuropeEast}},
	{"Myanmar Standard Time", {Area::SouthEastAsia}},
	{"SE Asia Standard Time", {Area::SouthEastAsia}},
	{"Altai Standard Time", {Area::EuropeEast}},
	{"W. Mongolia Standard Time", {Area::EastAsia}},
	{"North Asia Standard Time", {Area::EuropeEast}},
	{"N. Central Asia Standard Time", {Area::EuropeEast}},
	{"Tomsk Standard Time", {Area::EuropeEast}},
	{"China Standard Time", {Area::EastAsia}},
	{"North Asia East Standard Time", {Area::EastAsia}},
	{"Singapore Standard Time", {Area::SouthEastAsia}},
	{"W. Australia Standard Time", {Area::Oceania}},
	{"Taipei Standard Time", {Area::EastAsia}},
	{"Ulaanbaatar Standard Time", {Area::EastAsia}},
	{"Aus Central W. Standard Time", {Area::Oceania}},
	{"Transbaikal Standard Time", {Area::EastAsia}},
	{"Tokyo Standard Time", {Area::EastAsia, Area::SouthEastAsia, Area::Oceania}},
	{"North Korea Standard Time", {Area::EastAsia}},
	{"Korea Standard Time", {Area::EastAsia}},
	{"Yakutsk Standard Time", {Area::EastAsia}},
	{"Cen. Australia Standard Time", {Area::Oceania}},
	{"AUS Central Standard Time", {Area::Oceania}},
	{"E. Australia Standard Time", {Area::Oceania}},
	{"AUS Eastern Standard Time", {Area::Oceania}},
	{"West Pacific Standard Time", {Area::Oceania}},
	{"Tasmania Standard Time", {Area::Oceania}},
	{"Vladivostok Standard Time", {Area::EastAsia}},
	{"Lord Howe Standard Time", {Area::Oceania}},
	{"Bougainville Standard Time", {Area::Oceania}},
	{"Russia Time Zone 10", {Area::EastAsia}},
	{"Magadan Standard Time", {Area::EastAsia}},
	{"Norfolk Standard Time", {Area::Oceania}},
	{"Sakhalin Standard Time", {Area::EastAsia}},
	{"Central Pacific Standard Time", {Area::Oceania}},
	{"Russia Time Zone 11", {Area::EastAsia}},
	{"New Zealand Standard Time", {Area::Oceania}},
	{"UTC+12", {Area::Oceania}},
	{"Fiji Standard Time", {Area::Oceania}},
	{"Kamchatka Standard Time", {Area::EastAsia}},
	{"Chatham Islands Standard Time", {Area::Oceania}},
	{"UTC+13", {Area::Oceania}},
	{"Tonga Standard Time", {Area::Oceania}},
	{"Samoa Standard Time", {Area::Oceania}},
	{"Line Islands Standard Time", {Area::Oceania}},
};

// The area of each country of the flag table (Countries.hxx) that lies in one,
// in code order. A country across several areas has the one most of its
// players are in (the United States NA East, Canada NA East, Russia Europe
// East); its time zone usually says better. Antarctica and the uninhabited
// territories have none.
struct CountryArea { char code[3]; Area area; };
constexpr CountryArea Countries[] = {
	{"AD", Area::EuropeWest}, {"AE", Area::MiddleEastAfrica}, {"AF", Area::SouthAsia}, {"AG", Area::CentralAmerica},
	{"AI", Area::CentralAmerica}, {"AL", Area::EuropeEast}, {"AM", Area::EuropeEast}, {"AO", Area::MiddleEastAfrica},
	{"AR", Area::SouthAmerica}, {"AS", Area::Oceania}, {"AT", Area::EuropeWest}, {"AU", Area::Oceania},
	{"AW", Area::CentralAmerica}, {"AX", Area::EuropeEast}, {"AZ", Area::EuropeEast}, {"BA", Area::EuropeEast},
	{"BB", Area::CentralAmerica}, {"BD", Area::SouthAsia}, {"BE", Area::EuropeWest}, {"BF", Area::MiddleEastAfrica},
	{"BG", Area::EuropeEast}, {"BH", Area::MiddleEastAfrica}, {"BI", Area::MiddleEastAfrica}, {"BJ", Area::MiddleEastAfrica},
	{"BL", Area::CentralAmerica}, {"BM", Area::NaEast}, {"BN", Area::SouthEastAsia}, {"BO", Area::SouthAmerica},
	{"BQ", Area::CentralAmerica}, {"BR", Area::SouthAmerica}, {"BS", Area::CentralAmerica}, {"BT", Area::SouthAsia},
	{"BW", Area::MiddleEastAfrica}, {"BY", Area::EuropeEast}, {"BZ", Area::CentralAmerica}, {"CA", Area::NaEast},
	{"CC", Area::SouthEastAsia}, {"CD", Area::MiddleEastAfrica}, {"CF", Area::MiddleEastAfrica}, {"CG", Area::MiddleEastAfrica},
	{"CH", Area::EuropeWest}, {"CI", Area::MiddleEastAfrica}, {"CK", Area::Oceania}, {"CL", Area::SouthAmerica},
	{"CM", Area::MiddleEastAfrica}, {"CN", Area::EastAsia}, {"CO", Area::SouthAmerica}, {"CR", Area::CentralAmerica},
	{"CU", Area::CentralAmerica}, {"CV", Area::MiddleEastAfrica}, {"CW", Area::CentralAmerica}, {"CX", Area::SouthEastAsia},
	{"CY", Area::EuropeEast}, {"CZ", Area::EuropeEast}, {"DE", Area::EuropeWest}, {"DJ", Area::MiddleEastAfrica},
	{"DK", Area::EuropeWest}, {"DM", Area::CentralAmerica}, {"DO", Area::CentralAmerica}, {"DZ", Area::MiddleEastAfrica},
	{"EC", Area::SouthAmerica}, {"EE", Area::EuropeEast}, {"EG", Area::MiddleEastAfrica}, {"EH", Area::MiddleEastAfrica},
	{"ER", Area::MiddleEastAfrica}, {"ES", Area::EuropeWest}, {"ET", Area::MiddleEastAfrica}, {"FI", Area::EuropeEast},
	{"FJ", Area::Oceania}, {"FK", Area::SouthAmerica}, {"FM", Area::Oceania}, {"FO", Area::EuropeWest},
	{"FR", Area::EuropeWest}, {"GA", Area::MiddleEastAfrica}, {"GB", Area::EuropeWest}, {"GD", Area::CentralAmerica},
	{"GE", Area::EuropeEast}, {"GF", Area::SouthAmerica}, {"GG", Area::EuropeWest}, {"GH", Area::MiddleEastAfrica},
	{"GI", Area::EuropeWest}, {"GL", Area::NaEast}, {"GM", Area::MiddleEastAfrica}, {"GN", Area::MiddleEastAfrica},
	{"GP", Area::CentralAmerica}, {"GQ", Area::MiddleEastAfrica}, {"GR", Area::EuropeEast}, {"GS", Area::SouthAmerica},
	{"GT", Area::CentralAmerica}, {"GU", Area::Oceania}, {"GW", Area::MiddleEastAfrica}, {"GY", Area::SouthAmerica},
	{"HK", Area::EastAsia}, {"HN", Area::CentralAmerica}, {"HR", Area::EuropeEast}, {"HT", Area::CentralAmerica},
	{"HU", Area::EuropeEast}, {"ID", Area::SouthEastAsia}, {"IE", Area::EuropeWest}, {"IL", Area::MiddleEastAfrica},
	{"IM", Area::EuropeWest}, {"IN", Area::SouthAsia}, {"IO", Area::SouthAsia}, {"IQ", Area::MiddleEastAfrica},
	{"IR", Area::MiddleEastAfrica}, {"IS", Area::EuropeWest}, {"IT", Area::EuropeWest}, {"JE", Area::EuropeWest},
	{"JM", Area::CentralAmerica}, {"JO", Area::MiddleEastAfrica}, {"JP", Area::EastAsia}, {"KE", Area::MiddleEastAfrica},
	{"KG", Area::EuropeEast}, {"KH", Area::SouthEastAsia}, {"KI", Area::Oceania}, {"KM", Area::MiddleEastAfrica},
	{"KN", Area::CentralAmerica}, {"KP", Area::EastAsia}, {"KR", Area::EastAsia}, {"KW", Area::MiddleEastAfrica},
	{"KY", Area::CentralAmerica}, {"KZ", Area::EuropeEast}, {"LA", Area::SouthEastAsia}, {"LB", Area::MiddleEastAfrica},
	{"LC", Area::CentralAmerica}, {"LI", Area::EuropeWest}, {"LK", Area::SouthAsia}, {"LR", Area::MiddleEastAfrica},
	{"LS", Area::MiddleEastAfrica}, {"LT", Area::EuropeEast}, {"LU", Area::EuropeWest}, {"LV", Area::EuropeEast},
	{"LY", Area::MiddleEastAfrica}, {"MA", Area::MiddleEastAfrica}, {"MC", Area::EuropeWest}, {"MD", Area::EuropeEast},
	{"ME", Area::EuropeEast}, {"MF", Area::CentralAmerica}, {"MG", Area::MiddleEastAfrica}, {"MH", Area::Oceania},
	{"MK", Area::EuropeEast}, {"ML", Area::MiddleEastAfrica}, {"MM", Area::SouthEastAsia}, {"MN", Area::EastAsia},
	{"MO", Area::EastAsia}, {"MP", Area::Oceania}, {"MQ", Area::CentralAmerica}, {"MR", Area::MiddleEastAfrica},
	{"MS", Area::CentralAmerica}, {"MT", Area::EuropeWest}, {"MU", Area::MiddleEastAfrica}, {"MV", Area::SouthAsia},
	{"MW", Area::MiddleEastAfrica}, {"MX", Area::NaCentral}, {"MY", Area::SouthEastAsia}, {"MZ", Area::MiddleEastAfrica},
	{"NA", Area::MiddleEastAfrica}, {"NC", Area::Oceania}, {"NE", Area::MiddleEastAfrica}, {"NF", Area::Oceania},
	{"NG", Area::MiddleEastAfrica}, {"NI", Area::CentralAmerica}, {"NL", Area::EuropeWest}, {"NO", Area::EuropeWest},
	{"NP", Area::SouthAsia}, {"NR", Area::Oceania}, {"NU", Area::Oceania}, {"NZ", Area::Oceania},
	{"OM", Area::MiddleEastAfrica}, {"PA", Area::CentralAmerica}, {"PE", Area::SouthAmerica}, {"PF", Area::Oceania},
	{"PG", Area::Oceania}, {"PH", Area::SouthEastAsia}, {"PK", Area::SouthAsia}, {"PL", Area::EuropeEast},
	{"PM", Area::NaEast}, {"PN", Area::Oceania}, {"PR", Area::CentralAmerica}, {"PS", Area::MiddleEastAfrica},
	{"PT", Area::EuropeWest}, {"PW", Area::Oceania}, {"PY", Area::SouthAmerica}, {"QA", Area::MiddleEastAfrica},
	{"RE", Area::MiddleEastAfrica}, {"RO", Area::EuropeEast}, {"RS", Area::EuropeEast}, {"RU", Area::EuropeEast},
	{"RW", Area::MiddleEastAfrica}, {"SA", Area::MiddleEastAfrica}, {"SB", Area::Oceania}, {"SC", Area::MiddleEastAfrica},
	{"SD", Area::MiddleEastAfrica}, {"SE", Area::EuropeWest}, {"SG", Area::SouthEastAsia}, {"SH", Area::MiddleEastAfrica},
	{"SI", Area::EuropeEast}, {"SJ", Area::EuropeWest}, {"SK", Area::EuropeEast}, {"SL", Area::MiddleEastAfrica},
	{"SM", Area::EuropeWest}, {"SN", Area::MiddleEastAfrica}, {"SO", Area::MiddleEastAfrica}, {"SR", Area::SouthAmerica},
	{"SS", Area::MiddleEastAfrica}, {"ST", Area::MiddleEastAfrica}, {"SV", Area::CentralAmerica}, {"SX", Area::CentralAmerica},
	{"SY", Area::MiddleEastAfrica}, {"SZ", Area::MiddleEastAfrica}, {"TC", Area::CentralAmerica}, {"TD", Area::MiddleEastAfrica},
	{"TG", Area::MiddleEastAfrica}, {"TH", Area::SouthEastAsia}, {"TJ", Area::EuropeEast}, {"TK", Area::Oceania},
	{"TL", Area::SouthEastAsia}, {"TM", Area::EuropeEast}, {"TN", Area::MiddleEastAfrica}, {"TO", Area::Oceania},
	{"TR", Area::EuropeEast}, {"TT", Area::CentralAmerica}, {"TV", Area::Oceania}, {"TW", Area::EastAsia},
	{"TZ", Area::MiddleEastAfrica}, {"UA", Area::EuropeEast}, {"UG", Area::MiddleEastAfrica}, {"UM", Area::Oceania},
	{"US", Area::NaEast}, {"UY", Area::SouthAmerica}, {"UZ", Area::EuropeEast}, {"VA", Area::EuropeWest},
	{"VC", Area::CentralAmerica}, {"VE", Area::SouthAmerica}, {"VG", Area::CentralAmerica}, {"VI", Area::CentralAmerica},
	{"VN", Area::SouthEastAsia}, {"VU", Area::Oceania}, {"WF", Area::Oceania}, {"WS", Area::Oceania},
	{"YE", Area::MiddleEastAfrica}, {"YT", Area::MiddleEastAfrica}, {"ZA", Area::MiddleEastAfrica}, {"ZM", Area::MiddleEastAfrica},
	{"ZW", Area::MiddleEastAfrica},
};

// The country's area, None for one the table has none for.
inline Area AreaOfCountry(const CountryCode& country) {
	const std::string code = country.ToString();
	for (const auto& row : Countries) if (code == row.code) return row.area;
	return Area::None;
}

// The zone's row, or null for a key this table does not have.
inline const Zone* ZoneOf(const std::string& key) {
	for (const auto& zone : Zones) if (key == zone.key) return &zone;
	return nullptr;
}
} // namespace areas

// The area Automatic stands for: the time zone's own, or the country's when it
// is one of the zone's others; the country's for a zone with none of its own or
// one this build does not know; None when neither says.
inline Area AreaFromWindows(const std::string& timeZoneKey, const CountryCode& country) {
	const Area home = areas::AreaOfCountry(country);
	const areas::Zone* zone = areas::ZoneOf(timeZoneKey);
	if (!zone || zone->areas[0] == Area::None) return home;
	for (const Area area : zone->areas) if (area != Area::None && area == home) return home;
	return zone->areas[0];
}

}
