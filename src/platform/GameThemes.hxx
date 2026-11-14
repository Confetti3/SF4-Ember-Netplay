#pragma once
#include <cstdint>
#include <functional>
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
// What one part may hold when it is copied: the biggest real HUD and menu themes
// are a few hundred files and tens of megabytes, so these only stop a folder that
// would stall the game or fill the disk.
struct Limits { unsigned files = 2000; std::uintmax_t bytes = std::uintmax_t(512) << 20; };
// Why a part may not be used, or that it may. PicturesMismatch: its HUD layout and
// picture sheets do not belong together (the game closes on that); PicturesGuarded: the same
// mismatch where the game's picture read is guarded (Rules::pictureGuard), so the part is
// used and those pictures are drawn as none; Unreadable: a HUD file is not one the game can read.
enum class Verdict { Accepted, Refused, PicturesMismatch, PicturesGuarded, Unreadable };
// What a part must satisfy: its size, and for the battle HUD a check of its files. The check
// is given the theme's battle\hud folder; it knows the game's own files through whoever
// supplies it (hud_bundle::Check), so this folder of code stays free of the game's folders.
struct Rules {
    Limits limits;
    std::function<Verdict(const std::wstring& themeHudFolder, std::string& reason)> battleHud;
    // The game's picture read draws a picture its sheet lacks as none (sf4e::Game::Battle::Hud::PictureGuardInstalled),
    // so the files' mismatch is a note and not a refusal. Without it the mismatch refuses the part.
    bool pictureGuard = false;
};
// Whether a theme's part folder may be copied, and the plain reason when not. This is
// the one place a part's files are accepted or refused before they go in use: their
// size, links (which would copy what lies outside the theme), and for the battle HUD
// the check in the rules.
Verdict Accept(const wchar_t* part, const std::wstring& partFolder, std::string& reason, const Rules& rules = {});
// A theme or file name is one plain name: nothing that leaves its folder (no separator, drive or
// dot name), under 100 bytes, and UTF-8 a path can be made of. The phone and Settings both use this.
bool PlainName(const std::string& name);
// What Use did: the named part is in use (the game's own files for an empty
// name); the game's own files are, because the theme is gone or its part could
// not be copied, or was refused (PicturesMismatch, Unreadable: the refusals the player can
// do something about; GameOwn also covers the size limits); or nothing changed, because the
// part in use could not be moved aside (the game has one of its files open). Error says why
// for all but Used; UsedWithMissingPictures is in use too, and Error names the pictures.
enum class Outcome { Used, UsedWithMissingPictures, GameOwn, PicturesMismatch, Unreadable, Unchanged };
inline bool InUse(Outcome outcome) { return outcome == Outcome::Used || outcome == Outcome::UsedWithMissingPictures; }
// Makes ember\theme\<part> hold the named theme's part, or nothing for an
// empty name.
Outcome Use(const std::wstring& gameDirectory, const wchar_t* part, const std::string& name, std::string& error, const Rules& rules = {});
// Puts bytes at ember\theme\<part>\<file> (a plain relative path, no ".."),
// replacing what Use copied there; the game never reads a half-written file.
bool WriteActive(const std::wstring& gameDirectory, const wchar_t* part, const std::string& file, const std::string& bytes, std::string& error);
} } }
