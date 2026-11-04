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
    constexpr bool settingOn = true, settingOff = false, practice = true, ordinary = false;
    constexpr bool publicRoom = true, privateRoom = false;
    constexpr auto watching = Seat::Watching, fighting = Seat::Fighting;
    // A spectator's choice; a Practice match wants the meter with the setting off.
    CHECK(Wanted(settingOn, ordinary, watching) && Wanted(settingOff, practice, watching) && Wanted(settingOn, practice, watching));
    CHECK(!Wanted(settingOff, ordinary, watching));
    // A fighter never wants it from the setting, only in a Practice match.
    CHECK(!Wanted(settingOn, ordinary, fighting) && !Wanted(settingOff, ordinary, fighting));
    CHECK(Wanted(settingOff, practice, fighting) && Wanted(settingOn, practice, fighting));
    for (const bool replay : {false, true}) {
        CHECK(Watched(true, privateRoom, replay));
        CHECK(Watched(false, privateRoom, replay) == replay);
        CHECK(Watched(true, publicRoom, replay) == replay);
        CHECK(Watched(false, publicRoom, replay) == replay);
        // A private room's fighter with the setting on sees nothing; its spectator does.
        CHECK(Watched(Wanted(settingOn, ordinary, fighting), privateRoom, replay) == replay);
        CHECK(Watched(Wanted(settingOn, ordinary, watching), privateRoom, replay));
        // A Practice match in a private room shows it to its fighters with the
        // setting off, and a public room's still never does.
        CHECK(Watched(Wanted(settingOff, practice, fighting), privateRoom, replay));
        CHECK(Watched(Wanted(settingOff, practice, fighting), publicRoom, replay) == replay);
    }
    return 0;
}
