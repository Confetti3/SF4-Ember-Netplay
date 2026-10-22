#include "../common/ReplayMatchBoundary.hxx"
#include "test_support.hxx"
#include <vector>

namespace rp = sf4e::replay;
static rp::MatchBoundary Match(unsigned session, unsigned game, bool decided) {
 return {true, (std::uint64_t(session) << 32) | game, game == 1, decided};
}

// The native task checks, saves asynchronously, then stays idle. Three
// complete games must each reach that same native check exactly once.
static void TestFt3AndNextSet() {
 rp::MatchRecording recording;
 std::vector<std::uint64_t> saved;
 bool idle = false;
 unsigned starts = 0, ends = 0;
 for (unsigned session = 1; session <= 2; ++session) {
  idle = false; // The next set has a fresh native task, not a fresh owner.
  for (unsigned game = 1; game <= 3; ++game) {
   auto playing = Match(session, game, false);
   auto event = recording.Observe(playing);
   starts += event.start; ends += event.end;
   CHECK(!recording.Observe(playing).start);
   CHECK(recording.Route(playing, idle) != rp::SaveRoute::Check);
   auto result = Match(session, game, true);
   event = recording.Observe(result);
   starts += event.start; ends += event.end;
   CHECK(recording.Route(result, idle) == rp::SaveRoute::Check);
   // A busy recorder that has not created a save buffer is not consumed.
   CHECK(recording.Route(result, idle) == rp::SaveRoute::Check);
   saved.push_back(result.token);
   recording.Submitted(result);
   idle = true; // Native save completion returns to the idle callback.
   for (int tick = 0; tick < 20; ++tick) {
    CHECK(recording.Route(result, idle) == rp::SaveRoute::Idle);
    event = recording.Observe(result);
    CHECK(!event.start && !event.end);
   }
  }
 }
 CHECK(saved.size() == 6 && starts == 4 && ends == 6);
 CHECK(saved[0] != saved[3]); // Game one of a second set is a new recording.
}

static void TestConfirmedBoundaryAndOrdinaryFlow() {
 rp::MatchRecording recording;
 rp::MatchBoundary ordinary;
 CHECK(recording.Route(ordinary, false) == rp::SaveRoute::Check);
 CHECK(recording.Route(ordinary, true) == rp::SaveRoute::Idle);
 auto event = recording.Observe(ordinary);
 CHECK(!event.start && !event.end);
 rp::MatchBoundary unconfirmed{true};
 CHECK(recording.Route(unconfirmed, false) == rp::SaveRoute::Wait);
 CHECK(recording.Route(unconfirmed, true) == rp::SaveRoute::Idle);
 recording.Submitted(unconfirmed);
 auto first = Match(1, 1, true);
 event = recording.Observe(first);
 CHECK(!event.start && event.end);
 recording.Submitted(first);
 auto second = Match(1, 2, true);
 event = recording.Observe(second);
 CHECK(event.start && event.end);
 recording.Submitted(second);
 // An older timeline cannot reopen a recording, duplicate a save or end
 // a newer game's attribution. Returning through the first callback is safe.
 CHECK(recording.Route(first, false) == rp::SaveRoute::Wait);
 CHECK(recording.Route(first, true) == rp::SaveRoute::Idle);
 event = recording.Observe(first);
 CHECK(!event.start && !event.end);
 CHECK(recording.Route(second, false) == rp::SaveRoute::Wait);
 // Ending the session does not authorize an unconfirmed final save.
 CHECK(recording.Route(unconfirmed, false) == rp::SaveRoute::Wait);
}

int main() {
 TestFt3AndNextSet();
 TestConfirmedBoundaryAndOrdinaryFlow();
 return 0;
}
