// SF4's game catalog: USFIV's roster and its costume, colour and Ultra
// numbers, with SF4 Ember's own pick and stage rules (common/FighterCatalog,
// common/StageCatalog). Constant-initialized, so a static initializer can read
// it; the game catalog test pins every value. No generated header: the Linux
// room host compiles it with g++ alone (server/roomhost/sources.txt).

#include "../common/GameCatalog.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/StageCatalog.hxx"

namespace {
constexpr ember::GameCatalog Usf4 = {
	sf4e::selection::FighterCount,
	sf4e::selection::CostumeLimit,
	// Custom colours run up to the same limit as custom costumes (IsCustomColor).
	sf4e::selection::CostumeLimit,
	sf4e::selection::UltraChoices,
	&sf4e::selection::Valid,
	&sf4e::selection::ValidStage,
	{1, 6, 17, 24},
};
static_assert(ember::Usable(Usf4), "SF4's catalog must fit the shared code's limits");
}

namespace ember {
const GameCatalog& Catalog() { return Usf4; }
}
