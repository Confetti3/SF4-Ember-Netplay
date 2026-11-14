#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>

#include "../netplay/PlayerPreferences.hxx"

// In-game themes (Interface > In-game themes): the game searches <game>\ember\theme before its
// own folders, and the parts picked from themes are copied there
// (platform::game_themes). The game reads them the next time it loads those files.
// Everything here runs on the game thread.
namespace sf4e {
	namespace GameThemes {
		// Finds the themes in <game>\ember\themes (NetplayRuntime start-up).
		void Start(const std::wstring& gameDirectory);
		// Tells the game to search ember\theme once App::init, at the game's
		// first scene, has built its folder list: each runtime tick until then.
		void Search();
		// A pick that the game closed on while loading it, found at start-up: turned off for
		// that part (platform/GameThemeGuard.hxx).
		struct Turned { netplay::GameThemePart part; std::string theme; };
		// Start-up, before Use: puts the game's own back for the picks that were turned off.
		void TurnOff(std::array<std::string, netplay::GameThemePartCount>& picks);
		// The picks turned off that the settings and the player have yet to be told of, until TurnedOffTold.
		const std::vector<Turned>& TurnedOff();
		void TurnedOffTold();
		// What Settings tells the player about a theme for a part: the game closed on it
		// and it has not loaded since, or it was refused before use.
		netplay::GameThemeNote Note(netplay::GameThemePart part, const std::string& theme);
		// The themes with the part, by name.
		const std::vector<std::string>& Names(netplay::GameThemePart part);
		// Uses each part's theme whose name changed; an empty name is the
		// game's own. A main menu changed after the first pick, on the main
		// menu, opens the main menu again so it shows at once; other screens show
		// the next time they open; a battle HUD shows
		// from the next battle.
		// hudFiles (path under battle\hud -> bytes) are written over the battle HUD once it is copied.
		void Use(const std::array<std::string, netplay::GameThemePartCount>& names, const std::map<std::string, std::string>& hudFiles = {});
		// Game thread: the game has made a battle's HUD, so it is about to load the battle HUD theme in use
		// (the crash guard is armed for it; the fight state proves it).
		void BattleHudMade();
		// Any thread: shows a part's theme without saving it (the developer
		// picker where settings are locked, as in Training); a battle HUD in a
		// Training fight loads that battle again. The saved theme comes back
		// with the next Use.
		// With a flow row, the main menu then opens that row (0 the title
		// screen, 2 Versus mode, 4 Training), so the screen shows the preview.
		void Preview(netplay::GameThemePart part, const std::string& name, int mainMenuRow = -1);
		// Developer builds: the game's file opens in ember\theme since the last
		// preview, and the last one (CreateFile, hooked by Install).
		void Install();
		int ThemeFileOpens();
		std::string LastThemeFile();
		// Any thread: the developer page's verdict on a theme ("keep", "fix" or
		// "remove", empty for none), saved in ember\themes\review.json. A
		// theme judged "remove" moves to ember\themes-removed at the next tick.
		std::string Verdict(const std::string& name);
		void Judge(const std::string& name, const std::string& verdict);
		// The game folder, once Start has run.
		std::wstring Directory();
		// Any thread: uses the battle HUD of the named theme with files written over
		// it (HUD file name such as "cockpit" -> bytes of its .ems.emz, the editor's
		// layout applied), and loads a Training battle again to show it.
		void ApplyHud(const std::string& name, std::map<std::string, std::string> files);
		// Any thread: closes the game and starts it again (a Launcher that waits for this
		// one to exit), so name, character and portrait files of a theme load.
		void Restart();
		// Game thread, each runtime tick: uses the last preview asked for and
		// moves out the themes judged "remove".
		void TakePreview();
	}
}
