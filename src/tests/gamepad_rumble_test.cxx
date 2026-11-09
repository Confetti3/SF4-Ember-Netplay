#include "../common/GamepadRumble.hxx"
#include "test_support.hxx"
#include <initializer_list>
#include <utility>

int main() {
    using sf4e::gamepads::ShouldApplyRumble;
    CHECK(!ShouldApplyRumble(false, 0, 0));
    for (const auto motors : {std::pair<std::uint16_t, std::uint16_t>{12000, 0}, {0, 24000}, {12000, 24000}}) {
        bool pending = true;
        for (unsigned poll = 0; poll < 5; ++poll) {
            CHECK(ShouldApplyRumble(pending, motors.first, motors.second));
            pending = false; // No further XInputSetState calls while the game holds rumble.
        }
        CHECK(ShouldApplyRumble(true, 0, 0)); // An explicit stop reaches SDL once.
        CHECK(!ShouldApplyRumble(false, 0, 0));
    }
    // A layer polling faster than SDL needs refreshes held motors only when due;
    // a new request, a stop included, still goes at once.
    CHECK(!ShouldApplyRumble(false, 12000, 0, false));
    CHECK(ShouldApplyRumble(false, 12000, 0, true));
    CHECK(ShouldApplyRumble(true, 12000, 0, false));
    CHECK(ShouldApplyRumble(true, 0, 0, false));
    CHECK(!ShouldApplyRumble(false, 0, 0, true));
}
