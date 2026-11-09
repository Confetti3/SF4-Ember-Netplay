#pragma once
#include <cstdint>

namespace sf4e { namespace gamepads {
// SDL rumble expires unless a held motor request is refreshed: a new request
// (pending) is sent at once, held motors again whenever a refresh is due. A
// layer that polls once a frame refreshes on every poll (SF4 Ember); one that
// polls faster refreshes on a timer inside SDL's duration (SFV Ember).
inline bool ShouldApplyRumble(bool pending, std::uint16_t left, std::uint16_t right, bool refreshDue = true) {
    return pending || ((left || right) && refreshDue);
}
} }
