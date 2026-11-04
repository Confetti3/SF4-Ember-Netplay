// The match frame meter is watched by the player's choice for matches they
// watch in private rooms and outside rooms, never for one they fight, and
// always in a Practice match there; never in a public room. A replay's own
// meter is independent of all of them.
#include "../common/MatchFrameMeter.hxx"

#include "test_support.hxx"

int main() {
    using namespace sf4e::match_frame_meter;
    CHECK(Allowed(false));
    CHECK(!Allowed(true));
    // A spectator's choice; a Practice match wants the meter with the setting off.
    CHECK(Wanted(true, false, false) && Wanted(false, true, false) && Wanted(true, true, false));
    CHECK(!Wanted(false, false, false));
    // A fighter never wants it from the setting, only in a Practice match.
    CHECK(!Wanted(true, false, true) && !Wanted(false, false, true));
    CHECK(Wanted(false, true, true) && Wanted(true, true, true));
    for (int i = 0; i < 2; ++i) {
        const bool replay = i == 1;
        CHECK(Watched(true, false, replay));
        CHECK(Watched(false, false, replay) == replay);
        CHECK(Watched(true, true, replay) == replay);
        CHECK(Watched(false, true, replay) == replay);
        // A private room's fighter with the setting on sees nothing; its spectator does.
        CHECK(Watched(Wanted(true, false, true), false, replay) == replay);
        CHECK(Watched(Wanted(true, false, false), false, replay));
        // A Practice match in a private room shows it to its fighters with the
        // setting off, and a public room's still never does.
        CHECK(Watched(Wanted(false, true, true), false, replay));
        CHECK(Watched(Wanted(false, true, true), true, replay) == replay);
    }
    return 0;
}
