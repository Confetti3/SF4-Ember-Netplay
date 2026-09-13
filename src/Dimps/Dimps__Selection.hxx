#pragma once
#include <windows.h>
#include "../common/FighterCatalog.hxx"

namespace Dimps { namespace Selection {
void Locate(HMODULE executable);
std::uint32_t CostumeAvailabilityMask(std::uint32_t nativeFlags);
// Call on the game thread after the profile and DLC manager are initialized.
// No game state is changed. An unavailable profile produces ready == false.
sf4e::selection::Availability ReadAvailability(int fighterId);
} }
