// The areas (common/Area.hxx): the table's order and codes, which areas are
// near each other, the player's choice of area as it is saved and stepped, and
// the area Automatic stands for from Windows' time zone and country
// (common/AreaZones.hxx).
// The codes against ember_protocol's and the rooms page's are checked on the
// Rust side (`area::tests`), which reads the table.
#include "../common/Area.hxx"
#include "../common/AreaZones.hxx"
#include "test_support.hxx"
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <set>
#include <string>
#include <utility>

using namespace sf4e;

static void TableIsTheEnumInOrder() {
	std::set<std::string> codes;
	for (int i = 0; i < areas::Count; ++i) {
		const auto& row = areas::Table[i];
		CHECK(static_cast<int>(row.area) == i + 1);
		CHECK(codes.insert(row.code).second);
		CHECK(std::string(row.key) == std::string("area.") + row.code);
		CHECK(AreaFromCode(row.code) == row.area && AreaCode(row.area) == row.code && std::string(AreaKey(row.area)) == row.key);
		const std::string code = row.code;
		for (const char c : code) CHECK(c >= 'a' && c <= 'z');
	}
	CHECK(AreaCode(Area::None).empty() && AreaKey(Area::None) == nullptr);
	// A relay's code, a code in capitals or any other text is no area.
	for (const char* other : {"", "use1", "euc1", "aps1", "other", "NAW", "naw ", "na-w"}) CHECK(AreaFromCode(other) == Area::None);
}

static void NeighboursAreSymmetricAndEveryAreaHasOne() {
	std::set<std::pair<int, int>> pairs;
	for (const auto& pair : areas::Neighbours) {
		// Each pair once, of two different known areas.
		CHECK(pair.a != pair.b && pair.a != Area::None && pair.b != Area::None);
		const int a = static_cast<int>(pair.a), b = static_cast<int>(pair.b);
		CHECK(pairs.insert({(std::min)(a, b), (std::max)(a, b)}).second);
	}
	for (const auto& row : areas::Table) {
		int neighbours = 0;
		CHECK(AreaNear(row.area, row.area));
		for (const auto& other : areas::Table) {
			CHECK(AreaNear(row.area, other.area) == AreaNear(other.area, row.area));
			if (other.area != row.area && AreaNear(row.area, other.area)) ++neighbours;
		}
		CHECK(neighbours >= 1);
		CHECK(!AreaNear(row.area, Area::None) && !AreaNear(Area::None, row.area));
	}
	CHECK(!AreaNear(Area::None, Area::None));
	// What players see: the Americas, Europe with its neighbours, Asia and Oceania.
	CHECK(AreaNear(Area::NaWest, Area::NaCentral) && !AreaNear(Area::NaWest, Area::NaEast));
	CHECK(AreaNear(Area::SouthAmerica, Area::CentralAmerica) && !AreaNear(Area::SouthAmerica, Area::NaEast));
	CHECK(AreaNear(Area::EuropeWest, Area::EuropeEast) && AreaNear(Area::EuropeEast, Area::MiddleEastAfrica));
	CHECK(AreaNear(Area::SouthEastAsia, Area::Oceania) && !AreaNear(Area::EastAsia, Area::Oceania));
	CHECK(!AreaNear(Area::NaEast, Area::EuropeWest) && !AreaNear(Area::NaWest, Area::EastAsia));
}

static void PreferenceSavesStepsAndShows() {
	// Saved as "auto", "hidden" or a code; anything else is Automatic.
	CHECK(AreaPreference::Parsed("auto") == AreaPreference::Automatic());
	CHECK(AreaPreference::Parsed("hidden") == AreaPreference::Hidden());
	CHECK(AreaPreference::Parsed("eue") == AreaPreference::Of(Area::EuropeEast));
	for (const char* other : {"", "use1", "EUE", "Hidden", "none", "us-west"}) CHECK(AreaPreference::Parsed(other) == AreaPreference::Automatic());
	for (const auto& choice : {AreaPreference::Automatic(), AreaPreference::Hidden(), AreaPreference::Of(Area::Oceania)})
		CHECK(choice.Valid() && AreaPreference::Parsed(choice.ToString()) == choice);
	AreaPreference broken; broken.kind = AreaPreference::Kind::Chosen;
	CHECK(!broken.Valid() && AreaPreference::Of(Area::None) == AreaPreference::Automatic());
	broken = AreaPreference::Automatic(); broken.area = Area::EastAsia;
	CHECK(!broken.Valid());
	// Right steps Automatic, each area in order, then Hidden, and stops at either end;
	// a pick from the list is the steps to it.
	AreaPreference stepped;
	CHECK(stepped.Stepped(1) == AreaPreference::Of(Area::NaWest));
	CHECK(stepped.Stepped(-1) == AreaPreference::Automatic());
	CHECK(AreaPreference::Hidden().Stepped(1) == AreaPreference::Hidden());
	CHECK(AreaPreference::Of(Area::Oceania).Stepped(1) == AreaPreference::Hidden());
	CHECK(AreaPreference::Of(Area::NaEast).Stepped(-1) == AreaPreference::Of(Area::NaCentral));
	CHECK(stepped.Stepped(areas::Count + 1) == AreaPreference::Hidden() && stepped.Stepped(99) == AreaPreference::Hidden());
	CHECK(AreaPreference::Hidden().Stepped(-6) == AreaPreference::Of(Area::EuropeEast));
	int steps = 0;
	while (stepped != AreaPreference::Hidden()) { stepped = stepped.Stepped(1); CHECK(stepped.Valid()); ++steps; }
	CHECK(steps == areas::Count + 1);
	// Automatic shows Windows' area, a pick its own, Hidden none.
	CHECK(ShownArea(AreaPreference::Automatic(), Area::SouthAmerica) == Area::SouthAmerica);
	CHECK(ShownArea(AreaPreference::Automatic(), Area::None) == Area::None);
	CHECK(ShownArea(AreaPreference::Of(Area::EastAsia), Area::SouthAmerica) == Area::EastAsia);
	CHECK(ShownArea(AreaPreference::Hidden(), Area::SouthAmerica) == Area::None);
	// Hidden keeps the area from others only: the player's own screens still use a pick, else Windows'.
	CHECK(OwnArea(AreaPreference::Hidden(), Area::SouthAmerica) == Area::SouthAmerica && OwnArea(AreaPreference::Hidden(), Area::None) == Area::None);
	CHECK(OwnArea(AreaPreference::Of(Area::EastAsia), Area::SouthAmerica) == Area::EastAsia && OwnArea(AreaPreference::Automatic(), Area::SouthAmerica) == Area::SouthAmerica);
}

static void TablesOfZonesAndCountries() {
	// Every current Windows time zone once, each with distinct areas, the zone's own first.
	std::set<std::string> keys;
	for (const auto& zone : areas::Zones) {
		CHECK(keys.insert(zone.key).second);
		for (int i = 0; i < 3; ++i)
			for (int j = i + 1; j < 3; ++j) CHECK(zone.areas[j] == Area::None || zone.areas[i] != zone.areas[j]);
		CHECK(zone.areas[0] != Area::None || zone.areas[1] == Area::None);
	}
	CHECK(keys.size() == 141);
	// Countries of the flag table, in code order, each once; all but the four with nobody living there.
	std::string previous;
	for (const auto& row : areas::Countries) {
		CHECK(CountryCode::Parsed(row.code).Present() && previous < row.code && row.area != Area::None);
		previous = row.code;
	}
	CHECK(sizeof(areas::Countries) / sizeof(areas::Countries[0]) == static_cast<std::size_t>(countries::Count) - 4);
	for (const char* none : {"AQ", "BV", "HM", "TF"}) CHECK(areas::AreaOfCountry(CountryCode::Parsed(none)) == Area::None);
}

static void ZonesGiveTheirArea() {
	const auto none = CountryCode();
	const auto check = [&](Area area, std::initializer_list<const char*> zones) {
		for (const char* zone : zones) CHECK_MSG(AreaFromWindows(zone, none) == area, "%s", zone);
	};
	check(Area::NaWest, {"Pacific Standard Time", "Pacific Standard Time (Mexico)", "Mountain Standard Time", "US Mountain Standard Time",
		"Mountain Standard Time (Mexico)", "Alaskan Standard Time", "Aleutian Standard Time", "Hawaiian Standard Time", "Yukon Standard Time"});
	check(Area::NaCentral, {"Central Standard Time", "Canada Central Standard Time", "Central Standard Time (Mexico)", "Eastern Standard Time (Mexico)"});
	check(Area::NaEast, {"Eastern Standard Time", "US Eastern Standard Time", "Atlantic Standard Time", "Newfoundland Standard Time",
		"Greenland Standard Time", "Saint Pierre Standard Time"});
	check(Area::CentralAmerica, {"Central America Standard Time", "Cuba Standard Time", "Haiti Standard Time", "Turks And Caicos Standard Time"});
	check(Area::SouthAmerica, {"Montevideo Standard Time", "Argentina Standard Time", "E. South America Standard Time", "SA Pacific Standard Time",
		"SA Western Standard Time", "SA Eastern Standard Time", "Pacific SA Standard Time", "Venezuela Standard Time", "Paraguay Standard Time",
		"Bahia Standard Time", "Magallanes Standard Time", "Easter Island Standard Time", "Central Brazilian Standard Time", "Tocantins Standard Time"});
	check(Area::EuropeWest, {"W. Europe Standard Time", "GMT Standard Time", "Romance Standard Time", "Azores Standard Time"});
	check(Area::EuropeEast, {"Central Europe Standard Time", "Central European Standard Time", "FLE Standard Time", "GTB Standard Time",
		"E. Europe Standard Time", "Turkey Standard Time", "Russian Standard Time", "Belarus Standard Time", "Kaliningrad Standard Time",
		"Georgian Standard Time", "Caucasus Standard Time", "Azerbaijan Standard Time", "Ekaterinburg Standard Time", "Central Asia Standard Time"});
	check(Area::MiddleEastAfrica, {"Arab Standard Time", "Arabian Standard Time", "Arabic Standard Time", "Israel Standard Time", "Egypt Standard Time",
		"South Africa Standard Time", "E. Africa Standard Time", "W. Central Africa Standard Time", "Morocco Standard Time", "Iran Standard Time",
		"Jordan Standard Time", "Greenwich Standard Time", "Cape Verde Standard Time", "Mauritius Standard Time"});
	check(Area::SouthAsia, {"India Standard Time", "Pakistan Standard Time", "Bangladesh Standard Time", "Sri Lanka Standard Time",
		"Nepal Standard Time", "Afghanistan Standard Time"});
	check(Area::EastAsia, {"Tokyo Standard Time", "Korea Standard Time", "China Standard Time", "Taipei Standard Time", "Ulaanbaatar Standard Time",
		"North Korea Standard Time", "W. Mongolia Standard Time", "Vladivostok Standard Time", "Yakutsk Standard Time", "Magadan Standard Time",
		"Sakhalin Standard Time", "North Asia East Standard Time"});
	check(Area::SouthEastAsia, {"SE Asia Standard Time", "Singapore Standard Time", "Myanmar Standard Time"});
	check(Area::Oceania, {"AUS Eastern Standard Time", "E. Australia Standard Time", "Cen. Australia Standard Time", "AUS Central Standard Time",
		"W. Australia Standard Time", "Tasmania Standard Time", "New Zealand Standard Time", "Fiji Standard Time", "West Pacific Standard Time",
		"Tonga Standard Time", "Samoa Standard Time", "UTC+12", "Lord Howe Standard Time", "Chatham Islands Standard Time"});
	// Every area is some zone's own.
	for (const auto& row : areas::Table) {
		bool found = false;
		for (const auto& zone : areas::Zones) found = found || zone.areas[0] == row.area;
		CHECK(found);
	}
}

static void CountryBreaksTies() {
	const auto country = [](const char* code) { return CountryCode::Parsed(code); };
	// A zone across several areas takes the country's when it is one of them.
	CHECK(AreaFromWindows("SA Western Standard Time", country("PR")) == Area::CentralAmerica);
	CHECK(AreaFromWindows("SA Western Standard Time", country("BO")) == Area::SouthAmerica);
	CHECK(AreaFromWindows("SA Pacific Standard Time", country("JM")) == Area::CentralAmerica);
	CHECK(AreaFromWindows("SA Pacific Standard Time", country("PE")) == Area::SouthAmerica);
	CHECK(AreaFromWindows("Greenwich Standard Time", country("IS")) == Area::EuropeWest);
	CHECK(AreaFromWindows("Greenwich Standard Time", country("GH")) == Area::MiddleEastAfrica);
	CHECK(AreaFromWindows("Hawaiian Standard Time", country("US")) == Area::NaWest);
	CHECK(AreaFromWindows("Hawaiian Standard Time", country("PF")) == Area::Oceania);
	CHECK(AreaFromWindows("Tokyo Standard Time", country("TL")) == Area::SouthEastAsia);
	CHECK(AreaFromWindows("Central Asia Standard Time", country("CN")) == Area::EastAsia);
	CHECK(AreaFromWindows("West Asia Standard Time", country("MV")) == Area::SouthAsia);
	CHECK(AreaFromWindows("Eastern Standard Time", country("BS")) == Area::CentralAmerica);
	// A country outside the zone's areas does not move it: a player in the United States on Pacific time is in NA West.
	CHECK(AreaFromWindows("Pacific Standard Time", country("US")) == Area::NaWest);
	CHECK(AreaFromWindows("W. Europe Standard Time", country("BR")) == Area::EuropeWest);
	// A zone with no place of its own, or one this build does not know, is the country's.
	CHECK(AreaFromWindows("UTC", country("DE")) == Area::EuropeWest);
	CHECK(AreaFromWindows("Dateline Standard Time", country("UY")) == Area::SouthAmerica);
	CHECK(AreaFromWindows("Mars Standard Time", country("JP")) == Area::EastAsia);
	CHECK(AreaFromWindows("", country("AU")) == Area::Oceania);
	CHECK(AreaFromWindows("pacific standard time", country("CA")) == Area::NaEast);
	// Neither says: not set.
	CHECK(AreaFromWindows("Mars Standard Time", CountryCode()) == Area::None);
	CHECK(AreaFromWindows("UTC", CountryCode()) == Area::None);
	CHECK(AreaFromWindows("UTC", country("AQ")) == Area::None);
	CHECK(AreaFromWindows("", CountryCode()) == Area::None);
}

int main() {
	TableIsTheEnumInOrder();
	NeighboursAreSymmetricAndEveryAreaHasOne();
	PreferenceSavesStepsAndShows();
	TablesOfZonesAndCountries();
	ZonesGiveTheirArea();
	CountryBreaksTies();
	std::cout << "Area test passed\n";
	return 0;
}
