#pragma once
#include <windows.h>
#include "../common/FighterCatalog.hxx"
#include <functional>

namespace Dimps { namespace Selection {
void Locate(HMODULE executable);
std::uint32_t CostumeAvailabilityMask(std::uint32_t nativeFlags);
// Call on the game thread after the profile and DLC manager are initialized.
// No game state is changed. An unavailable profile produces ready == false.
sf4e::selection::Availability ReadAvailability(int fighterId);
// Tests only: ReadAvailability answers with `read` instead of the game; an
// empty one restores the game's.
void SetAvailabilityForTest(std::function<sf4e::selection::Availability(int)> read);
} }
