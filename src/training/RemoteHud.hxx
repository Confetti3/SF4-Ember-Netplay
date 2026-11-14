#pragma once
#include <functional>
#include <map>
#include <string>

// The HUD layout editor's side of the remote (docs/design/HUD_EDITOR.md): lists a
// theme's HUD elements, keeps its layout.json, and applies the layout to the
// running game. Desktop browsers only, and only after the player allows it.
// Knows nothing of sockets or of the running game: the Host does that part.
namespace sf4e { namespace training { namespace remote {
// One answer of the remote: its status, content type and body, and any more header lines (each ending "\r\n").
struct Reply { int status = 404; std::string type = "application/json", body, headers; };
namespace hud {
struct Host {
    std::function<std::wstring()> directory;   // the game folder
    // Uses the theme's battle HUD with these files (path under battle\hud such as "CMN/cockpit.ems.emz" -> bytes) over it.
    std::function<void(const std::string&, std::map<std::string, std::string>)> apply;
    std::function<void()> restart;   // closes the game and starts it again
};
// The editor's orders. The server's route table (RemoteRequest.hxx) gives each its address, method and body limit.
enum class Order { Themes, Theme, Scene, Sheet, Image, ImageRemove, Restart, SaveAs, Layout, Apply };

// The player's choice in the Training panel; off each time the remote starts.
void Allow(bool allowed);
bool Allowed();
// query: what follows "?" in the address (no "?"); body: a POST's; userAgent: the browser's.
Reply Handle(const Host& host, Order order, const std::string& query, const std::string& body, const std::string& userAgent);
} } } }
