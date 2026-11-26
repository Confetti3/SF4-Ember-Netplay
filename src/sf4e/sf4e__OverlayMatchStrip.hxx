#pragma once
// The match HUD's strip for a fight under way, as the overlay draws it: the
// netplay status the game thread published, through the player's HUD choices.
#include "sf4e__NetplayFacade.hxx"
#include "../ui/Theme.hxx"

namespace sf4e { namespace overlay {
// Class emblems off hides each fighter's mark mid-match too.
ui::MatchStripView MatchStripOf(const NetplayStatus& status, const netplay::PlayerPreferences& preferences);
} }
