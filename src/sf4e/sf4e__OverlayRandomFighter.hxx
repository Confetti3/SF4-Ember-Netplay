#pragma once
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__OverlayPrefs.hxx"
#include <string>

// Random on the fighter grid, for the overlay's own pick (docs/design/RANDOM_FIGHTER.md).
// The runtime owns the roll; the overlay tells it the pick and shows the roll.
namespace sf4e { namespace overlay {
// Drawing thread, every frame: notes the pick for the runtime
// (NetplayFacade::NoteRandomFighter), and takes each new roll it publishes
// into `lobby` (its fighter is `lobbyFighter`) in the rolled fighter's
// remembered look under `editionSelect`, as choosing it on the grid would.
void FollowRandomFighter(const NetplayFacade::RuntimeSnapshot& snapshot, const OverlayPrefs::Data& prefs,
                         Dimps::GameEvents::VsMode::ConfirmedCharaConditions& lobby, int& lobbyFighter, bool editionSelect);
// Training from Home plays the fighter in hand: a Random pick rolls again.
void RandomFighterPlayed();
// The pick's fighter as the player's own summaries name it: "Random · Ryu"
// for a Random pick, the fighter's name otherwise, or the choose prompt.
std::string PickName(int fighter, bool random);
} }
