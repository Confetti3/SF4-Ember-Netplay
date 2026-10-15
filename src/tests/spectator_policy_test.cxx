#include "../common/SpectatorPolicy.hxx"
#include <cstdio>
#include <cstdlib>

#include "test_support.hxx"
using sf4e::SpectatorPolicy;
using sf4e::SpectatorStreamWatch;

int main() {
	{
		// Initial sync: only unsynchronized spectators are dropped, and only
		// once the deadline, measured from the fighters' sync, has passed
		// without RUNNING.
		SpectatorPolicy policy;
		policy.Start(0, {1000, 1001, 1002});
		policy.OnFighterSynchronized(1000);
		policy.OnFighterSynchronized(1500); // only the first fighter sync counts
		policy.OnSynchronized(1001);
		CHECK(policy.SyncOverdue(1000 + SpectatorPolicy::SyncDeadlineMs - 1).empty());
		const auto overdue = policy.SyncOverdue(1000 + SpectatorPolicy::SyncDeadlineMs);
		CHECK((overdue == std::vector<int>{1000, 1002}));
		CHECK(policy.SyncOverdue(1000 + SpectatorPolicy::SyncDeadlineMs + 1).empty()); // each handle once
		CHECK((policy.Handles() == std::vector<int>{1001}));
	}
	{
		// No fighter has synchronized yet: the fight cannot start, so a
		// spectator that is still loading is never dropped for sync.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		CHECK(policy.SyncOverdue(3000).empty());
		CHECK(policy.SyncOverdue(SpectatorPolicy::SyncDeadlineMs).empty());
		CHECK(policy.SyncOverdue(SpectatorPolicy::SyncDeadlineMs * 10).empty());
		CHECK((policy.Handles() == std::vector<int>{1000}));
	}
	{
		// The fighters sync late and the spectator syncs within the deadline
		// after them: it is kept, although it synchronized long after the
		// session started.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		CHECK(policy.SyncOverdue(7999).empty());
		policy.OnFighterSynchronized(8000);
		CHECK(policy.SyncOverdue(11999).empty());
		policy.OnSynchronized(1000);
		CHECK(policy.SyncOverdue(12000).empty());
		CHECK(policy.SyncOverdue(8000 + SpectatorPolicy::SyncDeadlineMs * 10).empty());
		CHECK((policy.Handles() == std::vector<int>{1000}));
	}
	{
		// A spectator that never synchronizes is dropped exactly at the
		// deadline after the fighters' sync, and only once.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnFighterSynchronized(1000);
		CHECK(policy.SyncOverdue(1000 + SpectatorPolicy::SyncDeadlineMs - 1).empty());
		CHECK((policy.SyncOverdue(1000 + SpectatorPolicy::SyncDeadlineMs) == std::vector<int>{1000}));
		CHECK(policy.SyncOverdue(1000 + SpectatorPolicy::SyncDeadlineMs + 1).empty());
		CHECK(policy.Handles().empty());
	}
	{
		// A new session forgets the previous session's fighter sync time.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnFighterSynchronized(1000);
		policy.Start(20000, {1001});
		CHECK(policy.SyncOverdue(20000 + SpectatorPolicy::SyncDeadlineMs * 2).empty());
		CHECK((policy.Handles() == std::vector<int>{1001}));
		policy.OnFighterSynchronized(30000);
		CHECK(policy.SyncOverdue(30000 + SpectatorPolicy::SyncDeadlineMs - 1).empty());
		CHECK((policy.SyncOverdue(30000 + SpectatorPolicy::SyncDeadlineMs) == std::vector<int>{1001}));
	}
	{
		// RUNNING before the deadline means nobody is dropped for sync.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnFighterSynchronized(100);
		policy.OnRunning(200);
		CHECK(policy.SyncOverdue(SpectatorPolicy::SyncDeadlineMs * 10).empty());
	}
	{
		// Backlog: SlowSamples consecutive slow samples drop a spectator; a recovered
		// sample resets the count. Samples happen once per interval, only
		// once SyncDeadlineMs has passed since RUNNING.
		SpectatorPolicy policy;
		policy.Start(0, {1000, 1001});
		CHECK(!policy.SampleDue(5000)); // not running yet
		policy.OnSynchronized(1000); policy.OnSynchronized(1001); policy.OnRunning(1000);
		const auto due = 1000 + SpectatorPolicy::SyncDeadlineMs;
		CHECK(!policy.SampleDue(due - 1));
		CHECK(policy.SampleDue(due));
		CHECK(!policy.SampleDue(due + SpectatorPolicy::SampleIntervalMs - 1));
		CHECK(policy.SampleDue(due + SpectatorPolicy::SampleIntervalMs));
		const auto slowUntilLast = [&](int handle) {
			for (int i = 1; i < SpectatorPolicy::SlowSamples; ++i) CHECK(!policy.Sample(handle, SpectatorPolicy::SlowQueueFrames, 0));
		};
		slowUntilLast(1000);
		slowUntilLast(1001);
		CHECK(!policy.Sample(1001, SpectatorPolicy::SlowQueueFrames - 1, 0)); // caught up
		CHECK(policy.Sample(1000, SpectatorPolicy::SlowQueueFrames + 5, 0));
		CHECK(!policy.Sample(1000, 60, 0)); // already dropped
		slowUntilLast(1001);
		CHECK(policy.Sample(1001, SpectatorPolicy::SlowQueueFrames, 0));
		CHECK(policy.Handles().empty());
		CHECK(!policy.SampleDue(due + SpectatorPolicy::SampleIntervalMs * 5)); // nothing left to watch
	}
	{
		// A far spectator still finishing its end of the sync acknowledges
		// nothing while P1 streams. Inside the window after RUNNING no sample
		// is taken, so that backlog never drops it; GGPO's ring is the bound.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnSynchronized(1000); policy.OnRunning(1000);
		for (std::uint64_t now = 1000; now < 1000 + SpectatorPolicy::SyncDeadlineMs; now += 16)
			CHECK(!policy.SampleDue(now));
		CHECK((policy.Handles() == std::vector<int>{1000}));
	}
	{
		// The thresholds keep their floors up to the round trip that fills them,
		// then follow it: a round trip of frames plus one sample of growth, or
		// plus two for a drop at once.
		CHECK(SpectatorPolicy::FramesPerSample == 12);
		CHECK(SpectatorPolicy::InFlightFrames(-1) == 0); // not measured yet
		CHECK(SpectatorPolicy::SlowFrames(0) == SpectatorPolicy::SlowQueueFrames);
		CHECK(SpectatorPolicy::DropFrames(0) == SpectatorPolicy::DropQueueFrames);
		CHECK(SpectatorPolicy::SlowFrames(215) == SpectatorPolicy::SlowQueueFrames);
		CHECK(SpectatorPolicy::DropFrames(215) == SpectatorPolicy::DropQueueFrames);
		CHECK(SpectatorPolicy::InFlightFrames(650) == 39);
		CHECK(SpectatorPolicy::SlowFrames(650) == 51);
		CHECK(SpectatorPolicy::DropFrames(650) == 63);
		// A 650 ms spike keeps a healthy spectator 39 frames behind, which the
		// floors alone would drop after SlowSamples samples.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnRunning(1);
		for (int i = 0; i < SpectatorPolicy::SlowSamples * 4; ++i) CHECK(!policy.Sample(1000, 45, 650));
		CHECK(!policy.Sample(1000, 62, 650));
		CHECK(policy.Sample(1000, 63, 650));
		policy.Start(0, {1000});
		policy.OnRunning(1);
		for (int i = 1; i < SpectatorPolicy::SlowSamples; ++i) CHECK(!policy.Sample(1000, 45, 215));
		CHECK(policy.Sample(1000, 45, 215));
	}
	{
		// A spectator that stops acknowledging at 60 frames per second after
		// the window is dropped on the real sample cadence before GGPO's 63
		// frame ring fills, from a near round trip and from Waldo's 215 ms.
		for (const int pingMs : {0, 215}) {
			SpectatorPolicy policy;
			policy.Start(0, {1000});
			policy.OnSynchronized(1000); policy.OnRunning(1000);
			const auto stalledMs = 1000 + SpectatorPolicy::SyncDeadlineMs + 3000;
			const int inFlight = SpectatorPolicy::InFlightFrames(pingMs);
			int droppedAt = -1;
			for (int frame = inFlight; frame < 63 && droppedAt < 0; ++frame)
				if (policy.SampleDue(stalledMs + (frame - inFlight) * 1000ull / 60) && policy.Sample(1000, frame, pingMs))
					droppedAt = frame;
			CHECK(droppedAt > 0 && droppedAt <= 50);
		}
	}
	{
		// A game-thread hitch that delays the poll still drops on one sample.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnRunning(1);
		CHECK(policy.Sample(1000, SpectatorPolicy::DropQueueFrames, 0));
	}
	{
		// A spectator GGPO dropped on its own is forgotten.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnFighterSynchronized(100);
		policy.OnDisconnected(1000);
		CHECK(policy.SyncOverdue(100 + SpectatorPolicy::SyncDeadlineMs).empty());
	}
	{
		// A player enters a started match only from the main menu. A spectator
		// elsewhere waits until P1's sync window has passed, then sits the game
		// out; a fighter keeps waiting, and anyone at the menu enters.
		using Step = SpectatorPolicy::EntryStep;
		CHECK(SpectatorPolicy::EntryGate(true, true, 0) == Step::Enter);
		CHECK(SpectatorPolicy::EntryGate(true, false, SpectatorPolicy::SyncDeadlineMs * 10) == Step::Enter);
		CHECK(SpectatorPolicy::EntryGate(false, true, 0) == Step::Wait);
		CHECK(SpectatorPolicy::EntryGate(false, true, SpectatorPolicy::SyncDeadlineMs - 1) == Step::Wait);
		CHECK(SpectatorPolicy::EntryGate(false, true, SpectatorPolicy::SyncDeadlineMs) == Step::SitOut);
		CHECK(SpectatorPolicy::EntryGate(false, false, 0) == Step::Wait);
		CHECK(SpectatorPolicy::EntryGate(false, false, SpectatorPolicy::SyncDeadlineMs * 10) == Step::Wait);
	}
	{
		// A spectator client's own link: the stream ends once, exactly
		// StreamSyncTimeoutMs after its session started, as a lost stream.
		using Step = SpectatorStreamWatch::Step;
		const auto timeout = SpectatorPolicy::StreamSyncTimeoutMs;
		CHECK(timeout == 30000);
		SpectatorStreamWatch watch;
		CHECK(watch.Poll(timeout * 2, false) == Step::Wait); // no session yet
		watch.Start(1000, true);
		CHECK(watch.Poll(1000 + timeout - 1, false) == Step::Wait);
		CHECK(watch.Poll(1000 + timeout, false) == Step::EndLost);
		CHECK(watch.Poll(1000 + timeout + 1, false) == Step::Wait); // only once
		// A fresh session after that carries none of its time.
		watch.Start(50000, true);
		CHECK(watch.Poll(50000 + timeout - 1, false) == Step::Wait);
		CHECK(watch.Poll(50000 + timeout, false) == Step::EndLost);
		// RUNNING before the deadline ends the watch for the session.
		watch.Start(100000, true);
		watch.OnRunning();
		CHECK(watch.Poll(100000 + timeout * 10, false) == Step::Wait);
		// A known match result ends the stream without a notice.
		watch.Start(200000, true);
		CHECK(watch.Poll(200000 + timeout, true) == Step::EndQuietly);
		// A fighter's session is never watched, and starting one forgets a
		// spectator session that was still waiting.
		watch.Start(300000, true);
		watch.Start(300500, false);
		CHECK(watch.Poll(300000 + timeout * 10, false) == Step::Wait);
	}
	std::printf("Spectator policy passed\n");
	return 0;
}
