// SF4's game catalog (src/product/Sf4Catalog.cxx) against the numbers and
// rules the shared code checked picks by before it read a catalog. A
// difference here changes which fighters, looks and stages a room, the
// session protocol or the room host accepts from SF4 Ember players.

#include "../common/GameCatalog.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/StageCatalog.hxx"

#include "test_support.hxx"

namespace selection = sf4e::selection;

static void TestLimits() {
	const ember::GameCatalog& catalog = ember::Catalog();
	CHECK(catalog.fighterCount == 44 && catalog.fighterCount == selection::FighterCount);
	CHECK(catalog.costumeLimit == 99 && catalog.costumeLimit == selection::CostumeLimit);
	CHECK(catalog.colorLimit == 99 && catalog.colorLimit == selection::CostumeLimit);
	CHECK(catalog.ultraChoices == 3 && catalog.ultraChoices == selection::UltraChoices);
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

int main() {
	TestLimits();
	TestFighters();
	TestRules();
	return 0;
}
