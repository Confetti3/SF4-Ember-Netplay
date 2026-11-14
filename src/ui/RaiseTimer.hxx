#pragma once

namespace sf4e { namespace ui {
// A popout a press asked for, which a later frame opens on its row. `issued` once the screen has put the question on a
// row, `seen` once the player's choice is open.
struct RaiseTimer {
    bool issued = false, seen = false;
    int waits = 0;
    void Issue() { issued = true; seen = false; waits = 0; }
    // Counted once a frame, not by the queries that read it. `choosing`: the popout is open. False once the ask is over:
    // it was answered or closed, or it did not open in time.
    bool Tick(bool choosing) {
        if (!issued) return ++waits <= 8;
        if (choosing) seen = true;
        else if (seen || ++waits > 3) return false;
        return true;
    }
};
} }
