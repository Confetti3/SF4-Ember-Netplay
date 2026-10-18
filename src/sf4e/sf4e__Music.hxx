#pragma once
#include <string>

namespace sf4e { namespace Music {
// Install inside Sidecar's existing Detours transaction. No audio or file I/O
// runs under DllMain. Offsets/signatures target the Steam USFIV 1.05 executable.
void Install();
void Activate();
bool Available();
// Startup only, after DllMain. Inventory owned game's stock BGM files.
void InitializeContent();
// Game thread only, once per outer tick, never during rollback playback.
void Configure(int lobbyChoice, int matchChoice, bool safeMenu, bool inRoom);
// Arm only on an authorized online match entry, cleared on cancellation/end.
void EnterMatch();
void LeaveMatch();
// A same-choice save retries a bank that previously failed to load.
void RetryLobby();
// Copied to the immutable presentation snapshot on the game thread.
std::string Error();
} }
