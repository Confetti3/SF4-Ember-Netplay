#include "Dimps__Sound.hxx"

namespace Dimps { namespace Sound {
namespace {
void** soundSystem = nullptr;
int (__cdecl* play)(std::uint32_t, std::uint32_t) = nullptr;
int (__cdecl* isPlaying)(std::uint32_t) = nullptr;
void (__cdecl* stop)(std::uint32_t, int) = nullptr;
int (__cdecl* setPlayerScale)(std::uint32_t, float) = nullptr;
}

void Locate(HMODULE executable) {
    const auto base = reinterpret_cast<std::uintptr_t>(executable);
    // USFIV Steam 1.05. The boot sound setup at RVA 0x275690 stores the system
    // at 0x6ac348 with the GAME_SE sheet and its two players. The option menu
    // at RVA 0x224550 previews cues with IsPlaying, Stop(channel, 1), Play.
    soundSystem = reinterpret_cast<void**>(base + 0x6ac348);
    play = reinterpret_cast<int (__cdecl*)(std::uint32_t, std::uint32_t)>(base + 0x275640);
    isPlaying = reinterpret_cast<int (__cdecl*)(std::uint32_t)>(base + 0x275320);
    stop = reinterpret_cast<void (__cdecl*)(std::uint32_t, int)>(base + 0x2752f0);
    // Scales one player on top of master, slider and category volume, and
    // applies to a cue already playing. Every Play resets it to 1.
    setPlayerScale = reinterpret_cast<int (__cdecl*)(std::uint32_t, float)>(base + 0x275150);
}

bool PlaySystemCue(SystemCue cue, SystemChannel channel, float scale) {
    if (!soundSystem || !*soundSystem || !play || !isPlaying || !stop || !setPlayerScale) return false;
    const auto player = static_cast<std::uint32_t>(channel);
    if (isPlaying(player)) stop(player, 1);
    if (!play(static_cast<std::uint32_t>(cue), player)) return false;
    // The channel's player handles follow the sheet at +0x94.
    const auto handle = *reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(*soundSystem) + 0x94 + 4 * player);
    if (scale < 1.f) setPlayerScale(handle, scale < 0.f ? 0.f : scale);
    return true;
}
} }
