#include <windows.h>

#include <array>
#include <iterator>
#include <optional>

#include "spdlog/spdlog.h"

#include "../Dimps/Dimps.hxx"
#include "../platform/GameThemes.hxx"
#include "../platform/Utf8.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__GameThemes.hxx"

namespace {
std::wstring gameDirectory;
namespace game_themes = sf4e::platform::game_themes;
static_assert(std::size(game_themes::Parts) == sf4e::netplay::GameThemePartCount, "a folder for each part");
static_assert(game_themes::Parts[sf4e::netplay::GameThemeMenus] == game_themes::Menus &&
	game_themes::Parts[sf4e::netplay::GameThemeBattleHud] == game_themes::BattleHud, "the folders in part order");
struct Part {
	std::vector<std::string> names;
	// The theme last picked, once Use has run: in use, or settled as the game's
	// own when it is gone. Unknown while the part in use could not be swapped.
	std::optional<std::string> used;
};
std::array<Part, sf4e::netplay::GameThemePartCount> parts;
// Use has run once: what it does after is the player's pick.
bool started = false;
bool searched = false;

// Above the game's own patch folders: their priority (8), and a later order.
constexpr unsigned short Priority = 8, Order = 1;
}

void sf4e::GameThemes::Start(const std::wstring& directory) {
	gameDirectory = directory;
	for (int i = 0; i < netplay::GameThemePartCount; ++i) {
		parts[i].names = game_themes::Names(gameDirectory, game_themes::Parts[i]);
		spdlog::info("In-game themes: {} with {}", parts[i].names.size(), platform::WideToUtf8(game_themes::Parts[i]));
	}
}

void sf4e::GameThemes::Search() {
	void* const list = Dimps::App::resourcePathList ? *Dimps::App::resourcePathList : nullptr;
	if (searched || !list) return;
	using game_themes::ActiveFolder;
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

const std::vector<std::string>& sf4e::GameThemes::Names(netplay::GameThemePart part) { return parts[part].names; }

void sf4e::GameThemes::Use(const std::array<std::string, netplay::GameThemePartCount>& names) {
	if (gameDirectory.empty()) return;
	const bool picked = started;
	started = true;
	bool menusChanged = false;
	for (int i = 0; i < netplay::GameThemePartCount; ++i) {
		Part& part = parts[i];
		const std::string& name = names[i];
		if (part.used == name) continue;
		std::string error;
		const std::string folder = platform::WideToUtf8(game_themes::Parts[i]);
		const game_themes::Outcome outcome = game_themes::Use(gameDirectory, game_themes::Parts[i], name, error);
		if (outcome == game_themes::Outcome::Used)
			spdlog::info("In-game themes: {} {}", folder, name.empty() ? std::string("the game's own") : name);
		else spdlog::warn("In-game themes: {} {} could not be used: {}", folder, name, error);
		// Only a part that could not be swapped is tried again at the next pick.
		part.used = outcome == game_themes::Outcome::Unchanged ? std::nullopt : std::optional<std::string>(name);
		menusChanged |= i == netplay::GameThemeMenus && outcome != game_themes::Outcome::Unchanged;
	}
	// Menus picked on the main menu (the only place Settings can change them)
	// show at once: the menu opens again, as it does coming back from a mode.
	// A battle HUD is loaded by the next battle.
	if (picked && menusChanged && GameEvents::MainMenu::Open(Dimps::GameEvents::MainMenu::FlowRowMainMenuAgain))
		spdlog::info("In-game themes: main menu opened again");
}
