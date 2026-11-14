// The areas (common/Area.hxx): the table's order and codes, which areas are
// near each other, and the player's choice of area as it is saved and stepped.
// The codes against ember_protocol's and the rooms page's are checked on the
// Rust side (`area::tests`), which reads the table.
#include "../common/Area.hxx"
#include "test_support.hxx"
#include <algorithm>
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
}

int main() {
	TableIsTheEnumInOrder();
	NeighboursAreSymmetricAndEveryAreaHasOne();
	PreferenceSavesStepsAndShows();
	std::cout << "Area test passed\n";
	return 0;
}
