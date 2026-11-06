// SF4's game catalog: USFIV's roster and its costume, colour and Ultra
// numbers, with SF4 Ember's own pick, ownership and stage rules and its
// fighters' names and select-screen order (common/FighterCatalog,
// common/StageCatalog). Constant-initialized, so a static initializer can
// read it; the game catalog test pins every value. No generated header: the
// Linux room host compiles it with g++ alone (server/roomhost/sources.txt).

#include "../common/GameCatalog.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/InstalledContent.hxx"
#include "../common/RoomRules.hxx"
#include "../common/StageCatalog.hxx"

namespace {
// Random lands on the custom stages installed here as well as the game's.
int ResolveInstalledStage(int choice, std::uint32_t roll, const sf4e::selection::RandomPool& pool,
	const sf4e::selection::InstalledContent* installed) {
	return sf4e::selection::ResolveStage(choice, roll, pool, installed ? installed->stages : std::vector<int>());
}
const char* FighterName(int fighter) { return sf4e::selection::FindFighter(fighter)->name; }

constexpr ember::GameCatalog Usf4 = {
	sf4e::selection::FighterCount,
	sf4e::selection::CostumeLimit,
	// Custom colours run up to the same limit as custom costumes (IsCustomColor).
	sf4e::selection::CostumeLimit,
	sf4e::selection::UltraChoices,
	&sf4e::selection::Valid,
	&sf4e::selection::ValidStage,
	{1, 6, 17, 24},
	sf4e::selection::RandomStageId,
	&sf4e::selection::FighterLocked,
	&sf4e::selection::Available,
	&sf4e::selection::Normalize,
	&ResolveInstalledStage,
	&FighterName,
	&sf4e::selection::CostumeCount,
	sf4e::selection::RosterDisplayOrder,
	// Every rule a room offers: SF4 plays them all.
	sf4e::room::RoundCounts,
	sizeof(sf4e::room::RoundCounts) / sizeof(sf4e::room::RoundCounts[0]),
	sf4e::room::RoundTimes,
	sizeof(sf4e::room::RoundTimes) / sizeof(sf4e::room::RoundTimes[0]),
	true,
	true,
};
static_assert(ember::Usable(Usf4), "SF4's catalog must fit the shared code's limits");
}

namespace ember {
const GameCatalog& Catalog() { return Usf4; }
}
