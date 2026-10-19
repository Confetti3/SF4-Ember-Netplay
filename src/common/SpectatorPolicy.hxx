#pragma once

// When P1 drops a spectator from its GGPO session. Pure and game-free so it
// can be unit tested; fSystem feeds it GGPO events and network stats and
// performs the ggpo_disconnect_player calls outside GGPO callbacks.
//
// Two rules, both bounded so a spectator can never hold the fighters:
//  - initial sync: GGPO withholds RUNNING until every spectator has
//    synchronized. A spectator not synchronized SyncDeadlineMs after the
//    first fighter synchronized is dropped, which releases RUNNING. The
//    deadline is measured from the moment the fighters are synchronized,
//    because before that the fight cannot start anyway and a spectator's
//    late load costs the fighters nothing. If no fighter ever synchronizes,
//    the fighters' own GGPO disconnect tolerance ends the session, so no
//    spectator drop is needed.
//    At a tournament match's bound table the deadline is shorter
//    (BoundSyncDeadlineMs): a watcher is never worth more than that to a set.
//  - backlog: send_queue_len is the spectator's count of unacknowledged
//    frames. GGPO itself gives up at 63 (its pending-output ring). Sampling
//    starts SyncDeadlineMs after RUNNING: each end of a GGPO link
//    synchronizes on its own, so a far spectator can still be finishing its
//    end, acknowledging nothing, while P1 already streams. From then on a
//    spectator at SlowFrames or more on SlowSamples consecutive samples,
//    taken every SampleIntervalMs, is dropped. One sample at DropFrames drops
//    at once: a peer that stops acknowledging gains FramesPerSample frames
//    per sample, so waiting for a second could reach the ring. Both
//    thresholds grow with the measured round trip, since a healthy spectator
//    always has a round trip of frames unacknowledged, and never go below
//    SlowQueueFrames and DropQueueFrames. Where they reach the ring, GGPO's
//    own disconnect comes first.

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace sf4e {

class SpectatorPolicy {
public:
	static constexpr std::uint64_t SyncDeadlineMs = 5000;
	// A watcher of a tournament match's bound table: the fighters' set waits at
	// most this long for it to synchronize, so a slow or hostile watcher costs a
	// game start no more.
	static constexpr std::uint64_t BoundSyncDeadlineMs = 2000;
	static constexpr std::uint64_t SyncDeadlineFor(bool boundTable) { return boundTable ? BoundSyncDeadlineMs : SyncDeadlineMs; }
	static constexpr std::uint64_t SampleIntervalMs = 200;
	static constexpr int SlowQueueFrames = 32;
	static constexpr int DropQueueFrames = 48;
	// Five samples keep the one second a slow spectator always had to recover;
	// DropFrames, not this count, is what protects the ring.
	static constexpr int SlowSamples = 5;
	static constexpr int FramesPerSample = static_cast<int>(SampleIntervalMs * 60 / 1000);

	// Frames a healthy spectator leaves unacknowledged: one round trip of the
	// 60 frames per second stream. Zero until GGPO has measured the round trip.
	static int InFlightFrames(int pingMs) { return pingMs > 0 ? pingMs * 60 / 1000 : 0; }
	// The round trip plus one sample of growth kept up for SlowSamples, or
	// plus two in a single sample.
	static int SlowFrames(int pingMs) { return std::max(SlowQueueFrames, InFlightFrames(pingMs) + FramesPerSample); }
	static int DropFrames(int pingMs) { return std::max(DropQueueFrames, InFlightFrames(pingMs) + 2 * FramesPerSample); }

	// A spectator client gives up a stream whose own GGPO link has not reached
	// RUNNING this long after its session started. P1's session starts only
	// once P1 has loaded the battle, which on a slow disk or behind a slow
	// opponent can take well over SyncDeadlineMs, so this is generous: giving
	// up on a stream that would have come up is worse than a late notice. P1
	// ends the link of a spectator it drops, which ends that spectator's wait
	// within its peer-close grace; this bounds the wait when that end never
	// arrives. Without either, a spectator P1 dropped before it synchronized
	// would wait forever: P1 answers sync requests for 5 s after the drop,
	// then goes silent.
	static constexpr std::uint64_t StreamSyncTimeoutMs = 30000;

	// A player enters a started match only from the main menu. A spectator that
	// is elsewhere (Training, Options) for `deadlineMs` (SyncDeadlineFor, the
	// same one P1 waits by) has missed the window in which P1 waits for it, and
	// sits that game out instead of entering a stream P1 dropped. A fighter
	// keeps waiting: its opponent's GGPO disconnect tolerance bounds that.
	enum class EntryStep { Enter, Wait, SitOut };
	static EntryStep EntryGate(bool atMainMenu, bool spectator, std::uint64_t waitedMs, std::uint64_t deadlineMs) {
		if (atMainMenu) return EntryStep::Enter;
		return spectator && waitedMs >= deadlineMs ? EntryStep::SitOut : EntryStep::Wait;
	}

	// `syncDeadlineMs`: SyncDeadlineFor this game's table (SessionClient::SpectatorSyncDeadlineMs).
	void Start(std::uint64_t nowMs, const std::vector<int>& spectators, std::uint64_t syncDeadlineMs) {
		lastSampleMs_ = nowMs; fightersSyncedMs_ = 0; runningMs_ = 0; syncDeadlineMs_ = syncDeadlineMs;
		spectators_.clear();
		for (const auto handle : spectators) spectators_[handle] = State{};
	}
	void OnSynchronized(int handle) {
		const auto found = spectators_.find(handle);
		if (found != spectators_.end()) found->second.synchronized = true;
	}
	// The first fighter handle to synchronize starts the spectator deadline.
	void OnFighterSynchronized(std::uint64_t nowMs) {
		if (fightersSyncedMs_ == 0) fightersSyncedMs_ = nowMs;
	}
	void OnRunning(std::uint64_t nowMs) { runningMs_ = nowMs; }
	void OnDisconnected(int handle) { spectators_.erase(handle); }

	// Spectators to drop now because initial sync overran its deadline.
	std::vector<int> SyncOverdue(std::uint64_t nowMs) {
		std::vector<int> drop;
		if (runningMs_ || fightersSyncedMs_ == 0 || nowMs < fightersSyncedMs_ + syncDeadlineMs_) return drop;
		for (const auto& entry : spectators_) if (!entry.second.synchronized) drop.push_back(entry.first);
		for (const auto handle : drop) spectators_.erase(handle);
		return drop;
	}

	// True once per SampleIntervalMs from SyncDeadlineMs after RUNNING; the
	// caller then reports each spectator.
	bool SampleDue(std::uint64_t nowMs) {
		if (!runningMs_ || spectators_.empty() || nowMs < runningMs_ + SyncDeadlineMs ||
			nowMs < lastSampleMs_ + SampleIntervalMs) return false;
		lastSampleMs_ = nowMs;
		return true;
	}
	// Live spectator streams. Zero means there is nothing left to drain.
	std::size_t Count() const { return spectators_.size(); }
	std::vector<int> Handles() const {
		std::vector<int> handles;
		for (const auto& entry : spectators_) handles.push_back(entry.first);
		return handles;
	}
	// Returns true when this sample makes the spectator too slow to keep.
	bool Sample(int handle, int sendQueueFrames, int pingMs) {
		const auto found = spectators_.find(handle);
		if (found == spectators_.end()) return false;
		auto& state = found->second;
		state.slowSamples = sendQueueFrames >= SlowFrames(pingMs) ? state.slowSamples + 1 : 0;
		if (state.slowSamples < SlowSamples && sendQueueFrames < DropFrames(pingMs)) return false;
		spectators_.erase(found);
		return true;
	}

private:
	struct State { bool synchronized = false; int slowSamples = 0; };
	std::map<int, State> spectators_;
	// Zero until a fighter synchronizes, and until RUNNING, in this session.
	std::uint64_t fightersSyncedMs_ = 0, runningMs_ = 0, lastSampleMs_ = 0;
	std::uint64_t syncDeadlineMs_ = SyncDeadlineMs;
};

// A spectator client's watch over its own link to P1: the stream ends once,
// StreamSyncTimeoutMs after the session started, unless GGPO reached RUNNING
// first. Every session start resets it, and a fighter's session is never
// watched. fSystem ends the stream through AbortGgpoMatch.
class SpectatorStreamWatch {
public:
	enum class Step { Wait, EndQuietly, EndLost };
	void Start(std::uint64_t nowMs, bool spectator) { startedMs_ = nowMs; watching_ = spectator; }
	void OnRunning() { watching_ = false; }
	// Once the room has recorded the match's result, nothing is lost but the
	// tail of a finished match, so the stream ends without a notice.
	Step Poll(std::uint64_t nowMs, bool resultKnown) {
		if (!watching_ || nowMs < startedMs_ + SpectatorPolicy::StreamSyncTimeoutMs) return Step::Wait;
		watching_ = false;
		return resultKnown ? Step::EndQuietly : Step::EndLost;
	}

private:
	std::uint64_t startedMs_ = 0;
	bool watching_ = false;
};

// P1's spectator handles and the match slot each one watches from. A
// spectator GGPO drops, at the policy's request or on its own (its
// DISCONNECTED_FROM_PEER event, also from inside a callback), is noted here,
// and Take hands each dropped slot over once, outside callbacks, so its link
// can be ended. Start, for every session started or retired, forgets the last
// session's handles and any drop not yet taken.
class SpectatorLinks {
public:
	void Start(std::map<int, std::size_t> slots = {}) { slots_ = std::move(slots); dropped_.clear(); }
	void Dropped(int handle) {
		const auto found = slots_.find(handle);
		if (found == slots_.end()) return;
		dropped_.push_back(found->second);
		slots_.erase(found);
	}
	std::vector<std::size_t> Take() { std::vector<std::size_t> taken; taken.swap(dropped_); return taken; }

private:
	std::map<int, std::size_t> slots_;
	std::vector<std::size_t> dropped_;
};

}
