#pragma once

namespace sf4e { namespace ui {
// The screen the training HUD's windows cover: the meter, the shortcut strip
// over it and the input displays beside it, in the viewport's pixels. An area
// with no width covers nothing.
struct TrainingHudArea {
    float left = 0, top = 0, right = 0, bottom = 0;
    // Grows to cover other too.
    void Cover(const TrainingHudArea& other) {
        if (other.right <= other.left) return;
        if (right <= left) { *this = other; return; }
        left = left < other.left ? left : other.left; top = top < other.top ? top : other.top;
        right = right > other.right ? right : other.right; bottom = bottom > other.bottom ? bottom : other.bottom;
    }
};
// Passive, except for the chip that opens the controls.
// pointer: the pointer is over the chip, so the overlay takes
// the mouse (only) from the game. area: what the HUD drew, which the Data
// panel keeps clear of.
struct TrainingHudInput { bool open = false, pointer = false; TrainingHudArea area; };
// Bottom edge of the frame meter as a fraction of the viewport height: just
// above the game's super meters, where MeterOptions::position puts it by
// default, or where the player moved it. What sits under the meter (the keys
// notice, the replay strip) goes with it.
constexpr float TrainingHudBottom = .82f;
float TrainingMeterBottom();
// The HUD's windows (the meter's, the room line's, the input displays'): dark
// enough to read over a bright stage, which still shows through.
constexpr float TrainingHudBackground = .62f;
} }
