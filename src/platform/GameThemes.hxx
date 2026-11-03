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
// The parts. Menus: the game's ui folder, which menus and screens read and a
// match never does.
constexpr wchar_t Menus[] = L"ui";
// The themes in <game>\ember\themes that have the part, by name. Names are UTF-8.
std::vector<std::string> Names(const std::wstring& gameDirectory, const wchar_t* part);
// Makes ember\theme\<part> hold the named theme's part, or nothing for an
// empty name (the game's own files). A theme that is gone, or a part that
// cannot be copied, leaves the game's own files too, and is false with error
// saying why. When the part in use cannot be moved aside (the game has one of
// its files open), nothing changes: false, and error says so.
bool Use(const std::wstring& gameDirectory, const wchar_t* part, const std::string& name, std::string& error);
} } }
