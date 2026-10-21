#pragma once
#include <cstdint>

// Keeps Windows from taking the game for hung. The game reads its window
// messages each frame with a peek that leaves them queued (PM_NOREMOVE in its
// frame, 0x77CCA0) and takes them with GetMessage only when one is waiting.
// Windows does not count that peek as responding, so with only a pad in use no
// message arrives, and after 5 seconds it puts its "Not Responding" ghost in
// front of the window. The game then loses focus and, in full screen, minimizes
// itself; a player who clicks the ghost can have Windows close the game
// (exit 0xCFFFFFFF). A message posted about once a second sends the game
// through its own GetMessage, which Windows counts, while a game that really
// stops still goes unanswered and is ghosted as before.
namespace sf4e { namespace window_nudge {
constexpr std::uint64_t IntervalMs = 1000;

// When the next nudge is due: the first call, then IntervalMs after the last.
class Nudge {
public:
    // Whether a nudge is due at nowMs (a monotonic clock); records it when it is.
    bool Due(std::uint64_t nowMs) {
        if (sent_ && nowMs - last_ < IntervalMs) return false;
        sent_ = true;
        last_ = nowMs;
        return true;
    }
private:
    bool sent_ = false;
    std::uint64_t last_ = 0;
};
} }
