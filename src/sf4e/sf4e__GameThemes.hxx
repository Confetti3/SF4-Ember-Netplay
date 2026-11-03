#pragma once
#include <array>
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
		// The themes with the part, by name.
		const std::vector<std::string>& Names(netplay::GameThemePart part);
		// Uses each part's theme whose name changed; an empty name is the
		// game's own. Menus changed after the first pick, on the main menu,
		// open the main menu again so they show at once; a battle HUD shows
		// from the next battle.
		void Use(const std::array<std::string, netplay::GameThemePartCount>& names);
	}
}
