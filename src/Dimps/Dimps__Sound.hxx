#pragma once
#include <windows.h>
#include <cstdint>

namespace Dimps { namespace Sound {
// Rows of the resident system sheet (ui/sound/se/GAME_SE.csb, or
// dl0/GAME_AE_SE.csb; both keep these ids). It loads at boot and stays until
// exit, so it plays at the menus as well as in battle.
enum class SystemCue : std::uint32_t {
    HereComesChallenger = 15, // the announcer's "Here comes a new challenger!", about 3 s
};
// The two players the sheet owns: 0 for menu effects, 1 for the announcer.
enum class SystemChannel : std::uint32_t { Effects = 0, Voice = 1 };
void Locate(HMODULE executable);
// Game thread only: the sound system's player lists are unlocked. Restarts the
// channel's cue, as the game's own sound options do, at scale (0..1) times the
// game's volume for that channel. False when the sound system is not up.
bool PlaySystemCue(SystemCue cue, SystemChannel channel, float scale = 1.f);
} }
