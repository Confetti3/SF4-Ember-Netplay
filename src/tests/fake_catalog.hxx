#pragma once
// The test games' catalog (GameCatalog.hxx), with nothing of SF4's: a longer
// roster and more colours than SF4 has (60 fighters and 100 colours against
// 44 and 99), 20 costumes each, one Ultra, 40 stages and a Random stage that
// is not SF4's, no editions, and every table rule a room offers, Instant
// rematch too. Its copies can lack stages, as SFV's can. Its rules take any
// pick within the catalog's limits, read from Catalog(), so one set of rules
// serves every roster a test defines; its player owns everything. The fake
// game's Catalog() is TestCatalog(CatalogFighters) (fake_game.cxx);
// catalog_roster_test.cxx and the room missing-stage tests define their own
// from it.

#include "../common/GameCatalog.hxx"
#include "../common/RoomRules.hxx"

#include <cstdint>
#include <vector>

namespace ember { namespace test {

constexpr int CatalogFighters = 60;
constexpr int CatalogCostumes = 20;
constexpr int CatalogColors = 100;
constexpr int CatalogStages = 40;
// The choice that stands for Random: past the stages, and not SF4's 255.
constexpr int CatalogRandomStage = 99;

bool CatalogPick(const sf4e::selection::Pick& pick, bool editionSelect);
bool CatalogStage(std::int64_t id);
bool CatalogLocked(int fighter, const sf4e::selection::Availability& availability);
bool CatalogAvailable(const sf4e::selection::Pick& pick, bool editionSelect,
	const sf4e::selection::Availability& availability, const sf4e::selection::InstalledContent* installed);
bool CatalogNormalize(sf4e::selection::Pick& pick, bool editionSelect,
	const sf4e::selection::Availability* availability, const sf4e::selection::InstalledContent* installed);
// Random becomes the stage at `roll` among those `unavailable` leaves, or
// stays CatalogRandomStage when it leaves none; any other choice a valid
// stage, else 0.
int CatalogResolveStage(int choice, std::uint32_t roll, const sf4e::selection::RandomPool& pool,
	const sf4e::selection::InstalledContent* installed, const std::vector<std::int64_t>& unavailable);
// "Fighter 1" for fighter 0, and so on.
const char* CatalogFighterName(int fighter);
int CatalogCostumeCount(int fighter);
// In id order; a shorter roster reads its first entries.
extern const int CatalogRosterOrder[CatalogFighters];

// The catalog with `fighters` fighters, at most CatalogFighters.
constexpr GameCatalog TestCatalog(int fighters) {
	return {fighters, CatalogCostumes, CatalogColors, 1, &CatalogPick, &CatalogStage, {0, 11, 22, fighters - 1},
		CatalogRandomStage, &CatalogLocked, &CatalogAvailable, &CatalogNormalize, &CatalogResolveStage,
		&CatalogFighterName, &CatalogCostumeCount, CatalogRosterOrder,
		sf4e::room::RoundCounts, sizeof(sf4e::room::RoundCounts) / sizeof(sf4e::room::RoundCounts[0]),
		sf4e::room::RoundTimes, sizeof(sf4e::room::RoundTimes) / sizeof(sf4e::room::RoundTimes[0]), false, true, true};
}

} } // namespace ember::test
