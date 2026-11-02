#pragma once
#include "FighterCatalog.hxx"
#include <cstdint>
#include <functional>
#include <vector>

namespace sf4e { namespace selection {
// Random on the fighter grid: the player's pick is a fighter rolled for each
// game (room::RandomFighterPick owns it), each fighter they may play as
// likely as any other.

// The fighters a roll may land on: every fighter the player's Steam licences
// cover (FighterLocked, read through `availability`; none given locks none)
// that has a valid pick under the edition rule.
inline std::vector<int> RandomCandidates(const std::function<Availability(int)>& availability, bool editionSelect) {
    std::vector<int> fighters;
    for (int id = 0; id < FighterCount; ++id) {
        if (availability && FighterLocked(id, availability(id))) continue;
        Pick pick; pick.fighter = id;
        Normalize(pick, editionSelect);
        if (Valid(pick, editionSelect)) fighters.push_back(id);
    }
    return fighters;
}

// One of `candidates`, each as likely, from `draw`, a uniform 32-bit source.
// Values below 2^32 mod n are drawn again, so every candidate covers the same
// count of values. -1 when there are no candidates.
inline int DrawFighter(const std::vector<int>& candidates, const std::function<std::uint32_t()>& draw) {
    if (candidates.empty()) return -1;
    const auto count = static_cast<std::uint32_t>(candidates.size());
    const std::uint32_t uneven = (0u - count) % count;
    std::uint32_t value = draw();
    while (value < uneven) value = draw();
    return candidates[value % count];
}

// The pick a roll plays: the rolled fighter in the look last used on them
// (`remembered`, the per-fighter memory), fitted to the edition rule and to
// what is unlocked, as choosing that fighter on the grid would.
inline Pick RolledPick(int fighter, Pick remembered, bool editionSelect, const Availability* availability,
                       const InstalledContent* installed = nullptr) {
    remembered.fighter = fighter;
    Normalize(remembered, editionSelect, availability, installed);
    return remembered;
}
} }
