#include "../common/SpectatorPolicy.hxx"
#include <cstdio>
#include <cstdlib>

#include "test_support.hxx"
using sf4e::SpectatorPolicy;

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
		policy.OnRunning();
		CHECK(policy.SyncOverdue(SpectatorPolicy::SyncDeadlineMs * 10).empty());
	}
	{
		// Backlog: SlowSamples consecutive slow samples drop a spectator; a recovered
		// sample resets the count. Samples happen once per interval, only
		// while running.
		SpectatorPolicy policy;
		policy.Start(0, {1000, 1001});
		CHECK(!policy.SampleDue(5000)); // not running yet
		policy.OnSynchronized(1000); policy.OnSynchronized(1001); policy.OnRunning();
		CHECK(policy.SampleDue(SpectatorPolicy::SampleIntervalMs));
		CHECK(!policy.SampleDue(SpectatorPolicy::SampleIntervalMs + 1));
		const auto slowUntilLast = [&](int handle) {
			for (int i = 1; i < SpectatorPolicy::SlowSamples; ++i) CHECK(!policy.Sample(handle, SpectatorPolicy::SlowQueueFrames));
		};
		slowUntilLast(1000);
		slowUntilLast(1001);
		CHECK(!policy.Sample(1001, SpectatorPolicy::SlowQueueFrames - 1)); // caught up
		CHECK(policy.Sample(1000, SpectatorPolicy::SlowQueueFrames + 5));
		CHECK(!policy.Sample(1000, 60)); // already dropped
		slowUntilLast(1001);
		CHECK(policy.Sample(1001, SpectatorPolicy::SlowQueueFrames));
		CHECK(policy.Handles().empty());
		CHECK(!policy.SampleDue(SpectatorPolicy::SampleIntervalMs * 5)); // nothing left to watch
	}
	{
		// A spectator that stops acknowledging at 60 frames per second is dropped
		// on the real sample cadence before GGPO's 63 frame ring fills.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnSynchronized(1000); policy.OnRunning();
		int droppedAt = -1;
		for (int frame = 1; frame < 63 && droppedAt < 0; ++frame)
			if (policy.SampleDue(frame * 1000ull / 60) && policy.Sample(1000, frame)) droppedAt = frame;
		CHECK(droppedAt > 0 && droppedAt <= 50);
	}
	{
		// A game-thread hitch that delays the poll still drops on one sample.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnRunning();
		CHECK(policy.Sample(1000, SpectatorPolicy::DropQueueFrames));
	}
	{
		// A spectator GGPO dropped on its own is forgotten.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnFighterSynchronized(100);
		policy.OnDisconnected(1000);
		CHECK(policy.SyncOverdue(100 + SpectatorPolicy::SyncDeadlineMs).empty());
	}
	std::printf("Spectator policy passed\n");
	return 0;
}
