#pragma once
#include <string>
#include <cstdint>
#include "../common/MusicChoice.hxx"

namespace sf4e { namespace Music {
// Install inside Sidecar's existing Detours transaction. No audio or file I/O
// runs under DllMain. Offsets/signatures target the Steam USFIV 1.05 executable.
void Install();
void Activate();
bool Available();
// Startup only, after DllMain. Inventory owned game's stock BGM files.
void InitializeContent();
// After installation all state and native sound calls belong to the game
// thread. The renderer only reads the immutable runtime snapshot. Never call
// these from rollback playback.
void Configure(int menuChoice, int matchChoice, bool safeMenu);
music::ChoicesPtr InstalledChoices();
// Resolve only after the game accepts entry, and carry the concrete id with
// NextRequest. Preparation takes it once; the native BgmPlayer owns it next.
int ResolveMatchChoice();
void TakeBattleChoice(int resolvedChoice);
void ReleaseBattle();
// Optional continuous-match integration: call once per outer game tick with
// a monotonically increasing CONFIRMED game token, never a predicted epoch.
// A confirmed result preloads; the next confirmed game applies its bank.
// Zero/older tokens are ignored. This never controls simulation or readiness.
void ObserveMatch(std::uint64_t confirmedGame, bool decided);
// A same-choice save retries a bank that previously failed to load.
void RetryMenu();
// Copied to the immutable presentation snapshot on the game thread.
std::string Error();
} }
