#pragma once
#include <stdint.h>

namespace sf4e { namespace gameconfig {

// The game's block of display settings, 13 dwords. The graphics device keeps
// one, which BuildPresentParameters builds from (0x7719D0), and Main another,
// which WriteDisplayConfig saves (0x77D5F0); the framework's setter copies a
// whole block (0x77FF30). Its own header because Dimps builds as C++14.
struct DisplayBlock {
    uint32_t unknown0[3];
    uint32_t width, height;
    float refresh;
    uint32_t unknown6[6];
    // 1 for exclusive fullscreen, 0 for a window.
    uint32_t fullScreen;
};
static_assert(sizeof(DisplayBlock) == 0x34, "13 dwords");

} }
