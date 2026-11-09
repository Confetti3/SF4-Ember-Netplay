#pragma once

// The steps of a launch that belong to one game: how it starts with Ember's
// module inside it, what it needs before and after a run, and what else its
// launcher takes on its command line. launcher.cxx is the same program for
// every product and calls these where the game's own work goes. Each product
// defines them once, in its own source (SF4 Ember's: launcher/Sf4Launch.cxx),
// as it defines Product() and Catalog(); a launcher without them fails to
// link instead of starting a game it does not know.

#include <windows.h>

#include <string>
#include <vector>

namespace CLI { class App; }
namespace sf4e {
namespace crash { struct DumpChannel; }
namespace launcher { struct PersistedSettings; }
}

namespace ember { namespace launch {

// The options this game's launcher adds to the shared ones. Called before
// the command line is read.
void AddOptions(CLI::App& app);

// The launcher's log, under the logs folder, and the scope of its
// single-instance lock (platform/LauncherInstance.hxx). Read before the
// command line is parsed, so a product that varies them reads its own
// arguments. Empty scope: the one lock per user.
std::wstring LauncherLogName();
std::wstring InstanceScope();

// The settings the launcher loaded and whether --console asked for the
// game's diagnostic console, before the first start (SF4 Ember builds
// Sidecar's payload from them).
void Prepare(sf4e::launcher::PersistedSettings& settings, bool showConsole);

// The game's language code from its folder, or empty when it is unknown.
std::string GameLanguage(const std::wstring& gameDirectory);

// The libraries the game module imports by name, which a copy left in the
// game's search path would replace (GameLocator.hxx). Empty: nothing to scan.
std::vector<const wchar_t*> RuntimeLibraries();

// The replay file a replay link names, or empty when the game has no replays
// or the link names none.
std::string ResolveReplayLink(const std::string& uri);

// Before each start, with the folder the game was found in.
void BeforeStart(const std::wstring& gameDirectory);

struct StartRequest {
	std::wstring installRoot;     // the package folder (Launcher.exe's)
	std::wstring gameDirectory;   // the folder the game starts in
	std::wstring executable;      // the game's exe in that folder
	std::wstring module;          // the product's gameModule in the package
	std::string moduleAnsi;       // the same path in the ANSI code page, no substitution
	sf4e::crash::DumpChannel* dumps; // the crash dump channel, open or not
};

// Why a start failed, for the message the launcher shows. A product's own
// check before the start may refuse it with a reason the shared messages do
// not cover; otherwise the Win32 error picks a shared one.
enum class StartRefusal {
	None,             // the start itself failed: error
	SteamNotReady,    // Steam is not running with an account signed in
	UnsupportedBuild, // the executable is not the game build the product runs on
};
struct StartFailure {
	DWORD error = 0;
	StartRefusal refusal = StartRefusal::None;
	// UnsupportedBuild: the build the product runs on, as players name it.
	std::string supportedBuild;
};

// Starts the game with the module inside it, before any of the game's own
// code runs. Returns the game's process, or null with why in failure. Logs
// its own steps.
HANDLE StartGame(const StartRequest& request, StartFailure& failure);

// What the launcher offers a product's steps.
// Asks the player whether Steam may be restarted before the game starts
// (OK or Cancel, launcher.steam_restart_ask); true for OK.
bool ConfirmSteamRestart();

// After each StartGame: ran when the game started and has now exited, false
// when the start failed. Releases what the run held.
void GameEnded(bool ran);

} } // namespace ember::launch
