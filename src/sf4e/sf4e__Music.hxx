#pragma once
#include <string>
#include <cstdint>

namespace sf4e { namespace Music {
// Install inside Sidecar's existing Detours transaction. No audio or file I/O
// runs under DllMain. Offsets/signatures target the Steam USFIV 1.05 executable.
void Install();
void Activate();
bool Available();
// Startup only, after DllMain. Inventory owned game's stock BGM files.
void InitializeContent();
// Game thread only, once per outer tick, never during rollback playback.
void Configure(int menuChoice, int matchChoice, bool safeMenu);
// Arm only on an authorized online match entry, cleared on cancellation/end.
void EnterMatch();
void LeaveMatch();
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
