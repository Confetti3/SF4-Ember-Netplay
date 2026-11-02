#include "sf4e__OverlayRandomFighter.hxx"
#include "../common/Localization.hxx"
#include "../common/RandomFighter.hxx"
#include <spdlog/spdlog.h>

namespace sf4e { namespace overlay {
void FollowRandomFighter(const NetplayFacade::RuntimeSnapshot& snapshot, const OverlayPrefs::Data& prefs,
                         Dimps::GameEvents::VsMode::ConfirmedCharaConditions& lobby, int& lobbyFighter, bool editionSelect) {
    std::array<Dimps::GameEvents::VsMode::ConfirmedCharaConditions, selection::FighterCount> remembered{};
    for (int id = 0; id < selection::FighterCount; ++id) OverlayPrefs::ToConfirmed(remembered[id], prefs.fighters[id]);
    auto pick = lobby;
    pick.charaID = static_cast<std::uint8_t>(lobbyFighter);
    NetplayFacade::NoteRandomFighter(prefs.lobbyRandom, pick, remembered);
    // Each roll once; a look edited since on the same fighter stays.
    static std::uint64_t taken = 0;
    const int fighter = snapshot.randomFighter;
    if (!prefs.lobbyRandom || fighter < 0 || snapshot.randomRoll == taken) return;
    taken = snapshot.randomRoll;
    if (fighter == lobbyFighter) return;
    selection::ToNative(selection::RolledPick(fighter, selection::FromNative(remembered[fighter]), editionSelect,
        &snapshot.fighterAvailability[fighter], snapshot.installedContent.get()), lobby);
    lobbyFighter = fighter;
    spdlog::info("Fighter select: Random rolled {}", selection::FindFighter(fighter)->name);
}

void RandomFighterPlayed() { NetplayFacade::NoteRandomFighterPlayed(); }

std::string PickName(int fighter, bool random) {
    const auto* found = selection::FindFighter(fighter);
    if (!found) return loc::T("card.choose_fighter");
    return random ? loc::Tf("selection.random_fighter", found->name) : std::string(found->name);
}
} }
