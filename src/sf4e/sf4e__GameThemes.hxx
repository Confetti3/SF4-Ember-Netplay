#pragma once
#include <string>
#include <vector>

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
		// The themes with menus, by name.
		const std::vector<std::string>& Menus();
		// Uses a theme's menus when the name changed; an empty name is the
		// game's own. A change after the first, picked on the main menu,
		// opens the main menu again so it shows at once.
		void UseMenus(const std::string& name);
	}
}
