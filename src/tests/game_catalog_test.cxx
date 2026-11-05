// SF4's game catalog (src/product/Sf4Catalog.cxx) against the numbers and
// rules the shared code checked picks by before it read a catalog. A
// difference here changes which fighters, looks and stages a room, the
// session protocol or the room host accepts from SF4 Ember players.

#include "../common/GameCatalog.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/InstalledContent.hxx"
#include "../common/StageCatalog.hxx"

#include "test_support.hxx"

namespace selection = sf4e::selection;

static void TestLimits() {
	const ember::GameCatalog& catalog = ember::Catalog();
	CHECK(catalog.fighterCount == 44 && catalog.fighterCount == selection::FighterCount);
	CHECK(catalog.costumeLimit == 99 && catalog.costumeLimit == selection::CostumeLimit);
	CHECK(catalog.colorLimit == 99 && catalog.colorLimit == selection::CostumeLimit);
	CHECK(catalog.ultraChoices == 3 && catalog.ultraChoices == selection::UltraChoices);
	CHECK(ember::MaxFighters == 255);
	CHECK(catalog.showcaseFighters[0] == 1 && catalog.showcaseFighters[1] == 6 &&
		catalog.showcaseFighters[2] == 17 && catalog.showcaseFighters[3] == 24);
	for (int fighter : catalog.showcaseFighters) CHECK(selection::FindFighter(fighter) != nullptr);
}

static void TestFighters() {
	CHECK(!ember::ValidFighter(-1) && ember::ValidFighter(0) && ember::ValidFighter(43));
	CHECK(!ember::ValidFighter(44) && !ember::ValidFighter(INT64_MAX));
}

static void TestRules() {
	const ember::GameCatalog& catalog = ember::Catalog();
	CHECK(catalog.validPick == &selection::Valid && catalog.validStage == &selection::ValidStage);
	selection::Pick decapre;
	decapre.fighter = 43;
	CHECK(catalog.validPick(decapre, false));
	decapre.edition = 13;   // Decapre has no SFIV edition
	CHECK(!catalog.validPick(decapre, true));
	CHECK(catalog.validStage(0) && catalog.validStage(29) && !catalog.validStage(22));
	CHECK(catalog.validStage(selection::CustomStageId("C12")) && !catalog.validStage(selection::RandomStageId));
}

// A roster past the one-byte id is refused wherever a catalog is checked.
static void TestUsable() {
	CHECK(ember::Usable(ember::Catalog()));
	ember::GameCatalog catalog = ember::Catalog();
	catalog.fighterCount = ember::MaxFighters;
	CHECK(ember::Usable(catalog));
	catalog.fighterCount = ember::MaxFighters + 1;
	CHECK(!ember::Usable(catalog));
	catalog.fighterCount = 0;
	CHECK(!ember::Usable(catalog));
	catalog = ember::Catalog();
	catalog.validStage = nullptr;
	CHECK(!ember::Usable(catalog));
	catalog = ember::Catalog();
	catalog.costumeLimit = 0;
	CHECK(!ember::Usable(catalog));
	catalog = ember::Catalog();
	catalog.availablePick = nullptr;
	CHECK(!ember::Usable(catalog));
	catalog = ember::Catalog();
	catalog.rosterOrder = nullptr;
	CHECK(!ember::Usable(catalog));
}

// The ownership rules, the stage roll and the facts are SF4's own functions
// and tables, so the runtime and the shell judge and name SF4's picks as they
// did when they called them directly.
static void TestOwnershipAndFacts() {
	const ember::GameCatalog& catalog = ember::Catalog();
	CHECK(catalog.randomStage == selection::RandomStageId);
	CHECK(catalog.fighterLocked == &selection::FighterLocked && catalog.availablePick == &selection::Available &&
		catalog.normalizePick == &selection::Normalize && catalog.costumeCount == &selection::CostumeCount);
	for (int at = 0; at < selection::FighterCount; ++at) {
		CHECK(catalog.rosterOrder[at] == selection::RosterDisplayOrder[at]);
		CHECK(catalog.fighterName(at) == selection::FindFighter(at)->name);
	}
	selection::InstalledContent installed;
	installed.stages.push_back(selection::CustomStageId("C12"));
	const selection::RandomPool pool;
	for (std::uint32_t roll = 0; roll < 40; ++roll) {
		CHECK(catalog.resolveStage(selection::RandomStageId, roll, pool, nullptr) ==
			selection::ResolveStage(selection::RandomStageId, roll, pool));
		CHECK(catalog.resolveStage(selection::RandomStageId, roll, pool, &installed) ==
			selection::ResolveStage(selection::RandomStageId, roll, pool, installed.stages));
	}
	CHECK(catalog.resolveStage(11, 0, pool, nullptr) == 11 && catalog.resolveStage(22, 0, pool, nullptr) == 0);
}

int main() {
	TestLimits();
	TestUsable();
	TestFighters();
	TestRules();
	TestOwnershipAndFacts();
	return 0;
}
