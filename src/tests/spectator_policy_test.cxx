#include "../common/SpectatorPolicy.hxx"
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::printf("Failed line %d: %s\n", __LINE__, #c); std::exit(1); } } while (false)
using sf4e::SpectatorPolicy;

int main() {
	{
		// Initial sync: only unsynchronized spectators are dropped, and only
		// once the deadline has passed without RUNNING.
		SpectatorPolicy policy;
		policy.Start(1000, {1000, 1001, 1002});
		policy.OnSynchronized(1001);
		CHECK(policy.SyncOverdue(1000 + SpectatorPolicy::SyncDeadlineMs - 1).empty());
		const auto overdue = policy.SyncOverdue(1000 + SpectatorPolicy::SyncDeadlineMs);
		CHECK((overdue == std::vector<int>{1000, 1002}));
		CHECK(policy.SyncOverdue(1000 + SpectatorPolicy::SyncDeadlineMs + 1).empty()); // each handle once
		CHECK((policy.Handles() == std::vector<int>{1001}));
	}
	{
		// RUNNING before the deadline means nobody is dropped for sync.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnRunning();
		CHECK(policy.SyncOverdue(SpectatorPolicy::SyncDeadlineMs * 10).empty());
	}
	{
		// Backlog: two consecutive slow samples drop a spectator; a recovered
		// sample resets the count. Samples happen once per interval, only
		// while running.
		SpectatorPolicy policy;
		policy.Start(0, {1000, 1001});
		CHECK(!policy.SampleDue(5000)); // not running yet
		policy.OnSynchronized(1000); policy.OnSynchronized(1001); policy.OnRunning();
		CHECK(policy.SampleDue(SpectatorPolicy::SampleIntervalMs));
		CHECK(!policy.SampleDue(SpectatorPolicy::SampleIntervalMs + 1));
		CHECK(!policy.Sample(1000, SpectatorPolicy::SlowQueueFrames));
		CHECK(!policy.Sample(1001, SpectatorPolicy::SlowQueueFrames));
		CHECK(!policy.Sample(1001, SpectatorPolicy::SlowQueueFrames - 1)); // caught up
		CHECK(policy.Sample(1000, SpectatorPolicy::SlowQueueFrames + 5));
		CHECK(!policy.Sample(1000, 60)); // already dropped
		CHECK(!policy.Sample(1001, SpectatorPolicy::SlowQueueFrames));
		CHECK(policy.Sample(1001, SpectatorPolicy::SlowQueueFrames));
		CHECK(policy.Handles().empty());
		CHECK(!policy.SampleDue(SpectatorPolicy::SampleIntervalMs * 5)); // nothing left to watch
	}
	{
		// A spectator GGPO dropped on its own is forgotten.
		SpectatorPolicy policy;
		policy.Start(0, {1000});
		policy.OnDisconnected(1000);
		CHECK(policy.SyncOverdue(SpectatorPolicy::SyncDeadlineMs).empty());
	}
	std::printf("Spectator policy passed\n");
	return 0;
}
