#include "../common/ConfirmedCheckpoint.hxx"
#include "../session/MatchTeardownTiming.hxx"
#include <cstdio>
#include <cstdlib>
#include <limits>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #value); return 1; } } while (false)

int main() {
    using sf4e::session::MatchTeardownTiming;
    using namespace sf4e::statehash;

    MatchTeardownTiming teardown;
    teardown.RequestEnd();
    CHECK(teardown.Requested());
    // Native GGPO may own the socket for 121 seconds. The helper deadline
    // does not exist until the first post-GGPO close dispatch.
    CHECK(!teardown.HelperCloseDispatched());
    CHECK(!teardown.HelperTimedOut(31000, false));
    teardown.DispatchHelperClose(121000);
    CHECK(teardown.HelperDeadline() == 151000);
    teardown.DispatchHelperClose(121001);
    CHECK(teardown.HelperDeadline() == 151000);
    CHECK(!teardown.HelperTimedOut(151000, true)); // waiting for room result
    CHECK(teardown.HelperTimedOut(151000, false));

    // The spectator exit bound is armed once and never extended, and an
    // unarmed one never fires however late the clock is.
    CHECK(!teardown.SpectatorExitTimedOut((std::numeric_limits<std::uint64_t>::max)()));
    teardown.ArmSpectatorExit(121000);
    CHECK(!teardown.SpectatorExitTimedOut(135999));
    teardown.ArmSpectatorExit(130000);
    CHECK(teardown.SpectatorExitTimedOut(136000));
    teardown.ClearSpectatorExit();
    CHECK(!teardown.SpectatorExitTimedOut(136000));
    // A clock near the end of its range saturates instead of wrapping early.
    MatchTeardownTiming late;
    late.ArmSpectatorExit((std::numeric_limits<std::uint64_t>::max)() - 1);
    CHECK(late.SpectatorExitTimedOut((std::numeric_limits<std::uint64_t>::max)()));

    CHECK(!IsValidCheckpointStateFrame(-1));
    CHECK(!IsCheckpointCadenceFrame(-32768));
    CHECK(IsCheckpointCadenceFrame(32760));
    CHECK(CheckpointRingIndex(32760) == (32760 / CheckpointInterval) % CheckpointRingSize);
    CHECK(CheckpointRingIndex(-32760) == -1);
    CHECK(SpectatorCheckpointStateFrame(32759) == 32760);
    CHECK(SpectatorCheckpointStateFrame(-1) == -1);
    CHECK(SpectatorCheckpointStateFrame((std::numeric_limits<int>::max)()) == -1);
    // Exercise the production capture identity seam: a corrected rollback
    // recapture mutates the real publication state, rather than comparing a
    // value with itself.
    int frameIdx = -1, ggpoStateFrame = -1; bool sent = true;
    CHECK(PrepareCheckpointIdentity(frameIdx,ggpoStateFrame,sent,32760));
    CHECK(frameIdx==32760&&ggpoStateFrame==32760&&!sent);
    sent=true;
    CHECK(PrepareCheckpointIdentity(frameIdx,ggpoStateFrame,sent,32760));
    CHECK(frameIdx==32760&&ggpoStateFrame==32760&&!sent);
    CHECK(!PrepareCheckpointIdentity(frameIdx,ggpoStateFrame,sent,-32760));
    CHECK(IsConfirmedCheckpoint(32760, 32759));
    CHECK(!IsConfirmedCheckpoint(32760, 32758));
    std::puts("Native teardown timing and non-wrapping checkpoint identity passed");
    return 0;
}
