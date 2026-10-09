#pragma once

namespace sf4e { namespace display {

struct Layout {
    int x = 0, y = 0, width = 0, height = 0;
    bool Valid() const { return width > 0 && height > 0; }
};

// An exact 16:9 render area, centered in physical monitor pixels. Whole
// multiples prevent accidental aspect distortion on unusual desktop sizes.
inline Layout Fit16By9(int monitorWidth, int monitorHeight) {
    Layout out;
    if (monitorWidth <= 0 || monitorHeight <= 0) return out;
    const int scale = monitorWidth / 16 < monitorHeight / 9
        ? monitorWidth / 16 : monitorHeight / 9;
    if (scale < 1) return out;
    out.width = scale * 16;
    out.height = scale * 9;
    out.x = (monitorWidth - out.width) / 2;
    out.y = (monitorHeight - out.height) / 2;
    return out;
}

} }
