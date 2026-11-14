#pragma once
#include "GameThemes.hxx"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// The check a theme's battle HUD passes before it is used. The game draws a HUD layout
// (<name>.ems.emz, an #EMS) from the pictures of a sheet (<name>.tex.emz, an #EMB) by
// number, and does not check that a number is below the sheet's count: a layout
// that names a picture the sheet does not have closes the game as a battle loads. A theme that brings
// an older, shorter sheet and no layout of its own gets the game's newer layout, which does that.
// So every layout the game will read must use only pictures its sheet has, the files being
// the theme's where it has them and the game's own newest copy where it does not.
namespace sf4e { namespace platform { namespace hud_bundle {
// The game's own newest copy of a file of battle\hud, by its path under battle\hud
// ("CMN/cockpit.tex.emz"); empty when the game has none. The game's folders are not known here.
using GameFile = std::function<std::filesystem::path(const std::wstring& relative)>;

// Every pair <name>.ems.emz and <name>.tex.emz in the same folder, where one of the two
// is in the theme's folder, is read as the game will see it after the theme is in use. Accepted when
// each layout's highest picture number is below its sheet's picture count (a pair the game
// has no other half for is left alone). PicturesMismatch names the pair; Unreadable is a file
// that cannot be read as an #EMZ, #EMS or #EMB (too large, cut short, not inflating, offsets outside it).
game_themes::Verdict Check(const std::wstring& themeHudFolder, const GameFile& gameFile, std::string& reason);

// The reading half, on bytes: the picture count of a sheet and the highest picture number a
// layout uses (-1 for none), each from a plain file or one wrapped as #EMZ. False when
// the bytes are not such a file.
bool SheetPictures(const std::vector<std::uint8_t>& file, std::uint32_t& count);
bool LayoutHighestPicture(const std::vector<std::uint8_t>& file, std::int64_t& highest);
} } }
