// The test games' catalog rules and facts (fake_catalog.hxx).
#include "fake_catalog.hxx"

#include "../common/SelectionPick.hxx"

#include <string>
#include <vector>

namespace ember { namespace test {
namespace {
bool Within(int value, int limit) { return value >= 0 && value < limit; }
}

bool CatalogPick(const sf4e::selection::Pick& pick, bool) {
	const GameCatalog& catalog = Catalog();
	return Within(pick.fighter, catalog.fighterCount) && Within(pick.costume, catalog.costumeLimit) &&
		Within(pick.color, catalog.colorLimit) && Within(pick.ultra, catalog.ultraChoices);
}

bool CatalogStage(std::int64_t id) { return id >= 0 && id < CatalogStages; }

bool CatalogLocked(int, const sf4e::selection::Availability&) { return false; }

bool CatalogAvailable(const sf4e::selection::Pick& pick, bool editionSelect, const sf4e::selection::Availability&,
	const sf4e::selection::InstalledContent*) {
	return CatalogPick(pick, editionSelect);
}

bool CatalogNormalize(sf4e::selection::Pick& pick, bool, const sf4e::selection::Availability*,
	const sf4e::selection::InstalledContent*) {
	const GameCatalog& catalog = Catalog();
	const sf4e::selection::Pick before = pick;
	if (!Within(pick.fighter, catalog.fighterCount)) pick.fighter = 0;
	if (!Within(pick.costume, catalog.costumeLimit)) pick.costume = 0;
	if (!Within(pick.color, catalog.colorLimit)) pick.color = 0;
	if (!Within(pick.ultra, catalog.ultraChoices)) pick.ultra = 0;
	return pick.fighter != before.fighter || pick.costume != before.costume || pick.color != before.color ||
		pick.ultra != before.ultra;
}

int CatalogResolveStage(int choice, std::uint32_t, const sf4e::selection::RandomPool&,
	const sf4e::selection::InstalledContent*) {
	return CatalogStage(choice) ? choice : 0;
}

const char* CatalogFighterName(int fighter) {
	static const std::vector<std::string> names = [] {
		std::vector<std::string> all;
		for (int id = 0; id < CatalogFighters; ++id) all.push_back("Fighter " + std::to_string(id + 1));
		return all;
	}();
	return names.at(static_cast<std::size_t>(fighter)).c_str();
}

int CatalogCostumeCount(int) { return CatalogCostumes; }

const int CatalogRosterOrder[CatalogFighters] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
	20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
	40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59};

} } // namespace ember::test
