#pragma once
#include <functional>
#include <map>
#include <string>

// The HUD layout editor's side of the remote (docs/design/HUD_EDITOR.md): lists a
// theme's HUD elements, keeps its layout.json, and applies the layout to the
// running game. Desktop browsers only, and only after the player allows it.
// Knows nothing of sockets or of the running game: the Host does that part.
namespace sf4e { namespace training { namespace remote { namespace hud {
struct Host {
    std::function<std::wstring()> directory;   // the game folder
    // Uses the theme's battle HUD with these files (name such as "cockpit" -> .ems.emz bytes) over it.
    std::function<void(const std::string&, std::map<std::string, std::string>)> apply;
};
struct Reply { int status = 404; std::string type = "application/json", body; };

// The player's choice in the Training panel; off each time the remote starts.
void Allow(bool allowed);
bool Allowed();
bool Owns(const std::string& path);
// path is /api/hud/..., query what follows "?" (no "?"), userAgent the browser's.
Reply Handle(const Host& host, bool post, const std::string& path, const std::string& query, const std::string& body, const std::string& userAgent);
} } } }
