#pragma once

#include <string>
#include <vector>

namespace sf4e { namespace platform {

// Files dropped on the game's window: the window starts taking drops with
// AcceptDroppedFiles(window, true), and says it no longer does with false when
// Ember lets the window go. A drop does not reach an elevated game from an
// Explorer that is not elevated (Windows keeps lower-integrity senders out of
// a higher-integrity window), so there it does nothing, and nothing says so.
void AcceptDroppedFiles(void* window, bool accept);
// The paths in a WM_DROPFILES message's HDROP (wParam), UTF-8, at most the
// first 64; the drop is released. Handle the message once, from this.
std::vector<std::string> ReadDroppedFiles(void* drop);

} }
