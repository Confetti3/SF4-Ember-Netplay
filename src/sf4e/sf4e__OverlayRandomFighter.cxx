#include "sf4e__OverlayRandomFighter.hxx"
#include "../common/Localization.hxx"
#include "../common/RandomFighter.hxx"
#include "../session/RandomFighterWatch.hxx"
#include <random>
#include <spdlog/spdlog.h>

namespace sf4e { namespace overlay {
namespace {
// Only the drawing thread rolls, with its own engine.
room::RandomFighterWatch watch;
std::mt19937 rolls{std::random_device{}()};
}

void FollowRandomFighter(const NetplayFacade::RuntimeSnapshot& snapshot, const OverlayPrefs::Data& prefs,
                         Dimps::GameEvents::VsMode::ConfirmedCharaConditions& lobby, int& lobbyFighter, bool editionSelect) {
    static const room::Snapshot noRoom;
    const auto& room = snapshot.room ? *snapshot.room : noRoom;
    const bool winShown = snapshot.menuContext == input::MenuContext::MatchResult;
    if (watch.Update(prefs.lobbyRandom, room, winShown)) {
        const auto& owned = snapshot.fighterAvailability;
        const int fighter = selection::DrawFighter(selection::RandomCandidates([&](int id) { return owned[id]; }, editionSelect),
            [] { return static_cast<std::uint32_t>(rolls()); });
        if (fighter >= 0) {
            const auto pick = selection::RolledPick(fighter, selection::FromNative(prefs.fighters[fighter]), editionSelect,
                &owned[fighter], snapshot.installedContent.get());
            selection::ToNative(pick, lobby);
            lobbyFighter = fighter;
            spdlog::info("Fighter select: Random rolled {}", selection::FindFighter(fighter)->name);
        }
    }
    auto rolled = lobby;
    rolled.charaID = static_cast<std::uint8_t>(lobbyFighter);
    NetplayFacade::NoteRandomFighter(prefs.lobbyRandom, rolled);
}

void RandomFighterPlayed() { watch.Played(); }

std::string PickName(int fighter, bool random) {
    const auto* found = selection::FindFighter(fighter);
    if (!found) return loc::T("card.choose_fighter");
    return random ? loc::Tf("selection.random_fighter", found->name) : std::string(found->name);
}
} }
