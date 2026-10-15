// The match frame meter is watched by the player's choice in private rooms and
// outside rooms, and always in a Practice match there; never in a public room.
// A replay's own meter is independent of all of them.
#include "../common/MatchFrameMeter.hxx"

#include "test_support.hxx"

int main() {
    using namespace sf4e::match_frame_meter;
    CHECK(Allowed(false));
    CHECK(!Allowed(true));
    // A Practice match wants the meter with the setting off.
    CHECK(Wanted(true, false) && Wanted(false, true) && Wanted(true, true));
    CHECK(!Wanted(false, false));
    for (int i = 0; i < 2; ++i) {
        const bool replay = i == 1;
        CHECK(Watched(true, false, replay));
        CHECK(Watched(false, false, replay) == replay);
        CHECK(Watched(true, true, replay) == replay);
        CHECK(Watched(false, true, replay) == replay);
        // A Practice match in a private room shows it with the setting off,
        // and a public room's still never does.
        CHECK(Watched(Wanted(false, true), false, replay));
        CHECK(Watched(Wanted(false, true), true, replay) == replay);
    }
    return 0;
}
