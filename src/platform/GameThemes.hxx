#pragma once
#include <string>
#include <vector>

// In-game themes: replacement files for parts of the game's look, which the player puts
// in <game>\ember\themes\<theme>\<part>\..., each part laid out as the game's
// own folder of that name (docs/guides/GAME_THEMES.md). The game folder's own files
// are never changed: the part picked from a theme is copied to
// <game>\ember\theme\<part>, a folder the game searches before its own
// (sf4e::GameThemes), and the game loads it the next time it opens those files.
namespace sf4e { namespace platform { namespace game_themes {
// Where the themes are, and the folder the game is told to search, both
// relative to the game's.
constexpr wchar_t Folder[] = L"ember\\themes";
constexpr wchar_t ActiveFolder[] = L"ember\\theme";
// The parts, by folder, in netplay::GameThemePart order. The menu screens: one
// folder each of the game's ui folder, which menus and screens read and a match
// never does (advertise: the title screen, logos and demo; common: button
// prompts and icons). Battle HUD: the battle\hud folder, the life, super and
// revenge bars, timer and round calls a battle loads as it starts. Ember's desync
// check (StateHash) hashes the fighters and the battle flow, not the HUD.
constexpr wchar_t MainMenu[] = L"ui\\main_menu";
constexpr wchar_t BattleHud[] = L"battle\\hud";
constexpr const wchar_t* Parts[] = { MainMenu, L"ui\\advertise", L"ui\\chara_select", L"ui\\stage_select",
    L"ui\\versus", L"ui\\pause_menu", L"ui\\network_mode", L"ui\\edition_mark", L"ui\\common", L"ui\\sound", BattleHud };
// The themes in <game>\ember\themes that have the part, by name. Names are UTF-8.
std::vector<std::string> Names(const std::wstring& gameDirectory, const wchar_t* part);
// What Use did: the named part is in use (the game's own files for an empty
// name); the game's own files are, because the theme is gone or its part could
// not be copied; or nothing changed, because the part in use could not be moved
// aside (the game has one of its files open). Error says why for the last two.
enum class Outcome { Used, GameOwn, Unchanged };
// Makes ember\theme\<part> hold the named theme's part, or nothing for an
// empty name.
Outcome Use(const std::wstring& gameDirectory, const wchar_t* part, const std::string& name, std::string& error);
} } }
