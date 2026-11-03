#include <windows.h>

#include <optional>

#include "spdlog/spdlog.h"

#include "../Dimps/Dimps.hxx"
#include "../platform/GameThemes.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__GameThemes.hxx"

namespace {
std::wstring gameDirectory;
std::vector<std::string> menus;
// The menus theme in use, once UseMenus has run.
std::optional<std::string> used;
bool searched = false;

// Above the game's own patch folders: their priority (8), and a later order.
constexpr unsigned short Priority = 8, Order = 1;
}

void sf4e::GameThemes::Start(const std::wstring& directory) {
	gameDirectory = directory;
	menus = platform::game_themes::Names(gameDirectory, platform::game_themes::Menus);
	spdlog::info("In-game themes: {} with menus", menus.size());
}

void sf4e::GameThemes::Search() {
	void* const list = Dimps::App::resourcePathList ? *Dimps::App::resourcePathList : nullptr;
	if (searched || !list) return;
	using platform::game_themes::ActiveFolder;
	Dimps::App::ShortString folder = {};
	constexpr unsigned size = sizeof(ActiveFolder) / sizeof(*ActiveFolder) - 1;
	static_assert(size < sizeof(folder.text), "the folder's name fits the short form");
	for (unsigned i = 0; i < size; ++i) folder.text[i] = static_cast<char>(ActiveFolder[i]);
	folder.size = size;
	folder.capacity = 15;
	Dimps::App::AddResourcePath(list, Priority, &folder, Order);
	searched = true;
	spdlog::info("In-game themes: the game searches ember\\theme first");
}

const std::vector<std::string>& sf4e::GameThemes::Menus() { return menus; }

void sf4e::GameThemes::UseMenus(const std::string& name) {
	if (gameDirectory.empty() || used == name) return;
	const bool picked = used.has_value();
	used = name;
	std::string error;
	if (platform::game_themes::Use(gameDirectory, platform::game_themes::Menus, name, error))
		spdlog::info("In-game themes: menus {}", name.empty() ? std::string("the game's own") : name);
	else spdlog::warn("In-game themes: menus {} could not be used: {}", name, error);
	// A theme picked on the main menu (the only place Settings can change it)
	// shows at once: the menu opens again, as it does coming back from a mode.
	if (picked && GameEvents::MainMenu::Open(Dimps::GameEvents::MainMenu::FlowRowMainMenuAgain))
		spdlog::info("In-game themes: main menu opened again");
}
