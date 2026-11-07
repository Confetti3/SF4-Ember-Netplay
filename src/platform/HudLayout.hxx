#pragma once
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// HUD layout editing (docs/design/HUD_EDITOR.md): moves, rotates, scales and
// hides the elements of a theme's battle HUD by patching the sprite tracks of
// its #EMS files. The theme's own files are never written; Apply returns the
// bytes for the active folder.
namespace sf4e { namespace platform { namespace hud_layout {
// One piece of the HUD the editor moves: the sprite nodes of one EMS file whose names match.
struct Element {
    std::string id, label, file, pattern, parent;   // file empty for a group of other elements
};
const std::vector<Element>& Elements();
// The HUD files the elements live in (battle\hud\CMN\<name>.ems.emz).
const std::vector<std::string>& Files();

// Where an element is on the 1280x720 canvas, as the file has it.
struct Info { std::string id, label, parent; double cx = 0, cy = 0, w = 0, h = 0; };
// What the user did to an element: its stock centre goes to (x, y), then
// rotation (clockwise degrees) and scale about it. A group moves its elements.
struct Edit { double x = 0, y = 0, rot = 0, scale = 1; bool hide = false; };
using Layout = std::map<std::string, Edit>;

// layout.json beside a theme: {"p1_name": {"x": 90, "y": 73, "rot": -10, "s": 0.87, "hide": false}, ...}.
// An entry with a missing or bad number is left out; ids that are not elements are left out.
Layout LayoutFromJson(const nlohmann::json& json);
nlohmann::json LayoutToJson(const Layout& layout);
// Whether a browser's User-Agent is a desktop one (phones and tablets are not).
bool DesktopAgent(const std::string& userAgent);

// #EMZ (a zlib stream behind a 16 byte header) from and to the bytes of the #EMS inside.
bool Unpack(const std::string& emz, std::string& ems);
std::string Pack(const std::string& ems);

// emz: file name ("cockpit") -> bytes of its .ems.emz. Elements without nodes are left out.
std::vector<Info> Describe(const std::map<std::string, std::string>& emz);
// The same files with the layout applied (only the files an edit touched are returned).
// False when a file cannot be read.
bool Apply(const std::map<std::string, std::string>& emz, const Layout& layout, std::map<std::string, std::string>& out);
} } }
