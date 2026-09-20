#pragma once

// When P1 drops a spectator from its GGPO session. Pure and game-free so it
// can be unit tested; fSystem feeds it GGPO events and network stats and
// performs the ggpo_disconnect_player calls outside GGPO callbacks.
//
// Two rules, both bounded so a spectator can never hold the fighters:
//  - initial sync: GGPO withholds RUNNING until every spectator has
//    synchronized. A spectator not synchronized SyncDeadlineMs after the
//    session started is dropped, which releases RUNNING.
//  - backlog: send_queue_len is the spectator's count of unacknowledged
//    frames. GGPO itself only gives up at 63 (its pending-output ring). A
//    spectator at SlowQueueFrames or more on SlowSamples consecutive samples,
//    taken every SampleIntervalMs, is dropped well before that. One sample at
//    DropQueueFrames drops at once: a peer that stops acknowledging gains 12
//    frames per sample, so waiting for a second could reach the ring.

#include <cstdint>
#include <map>
#include <vector>

namespace sf4e {

class SpectatorPolicy {
public:
	static constexpr std::uint64_t SyncDeadlineMs = 3000;
	static constexpr std::uint64_t SampleIntervalMs = 200;
	static constexpr int SlowQueueFrames = 32;
	static constexpr int DropQueueFrames = 48;
	// Five samples keep the one second a slow spectator always had to recover;
	// DropQueueFrames, not this count, is what protects the ring.
	static constexpr int SlowSamples = 5;

	void Start(std::uint64_t nowMs, const std::vector<int>& spectators) {
		startedMs_ = nowMs; lastSampleMs_ = nowMs; running_ = false;
		spectators_.clear();
		for (const auto handle : spectators) spectators_[handle] = State{};
	}
	void OnSynchronized(int handle) {
		const auto found = spectators_.find(handle);
		if (found != spectators_.end()) found->second.synchronized = true;
	}
	void OnRunning() { running_ = true; }
	void OnDisconnected(int handle) { spectators_.erase(handle); }

	// Spectators to drop now because initial sync overran its deadline.
	std::vector<int> SyncOverdue(std::uint64_t nowMs) {
		std::vector<int> drop;
		if (running_ || nowMs < startedMs_ + SyncDeadlineMs) return drop;
		for (const auto& entry : spectators_) if (!entry.second.synchronized) drop.push_back(entry.first);
		for (const auto handle : drop) spectators_.erase(handle);
		return drop;
	}

	// True once per SampleIntervalMs; the caller then reports each spectator.
	bool SampleDue(std::uint64_t nowMs) {
		if (!running_ || spectators_.empty() || nowMs < lastSampleMs_ + SampleIntervalMs) return false;
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
	bool Sample(int handle, int sendQueueFrames) {
		const auto found = spectators_.find(handle);
		if (found == spectators_.end()) return false;
		auto& state = found->second;
		state.slowSamples = sendQueueFrames >= SlowQueueFrames ? state.slowSamples + 1 : 0;
		if (state.slowSamples < SlowSamples && sendQueueFrames < DropQueueFrames) return false;
		spectators_.erase(found);
		return true;
	}

private:
	struct State { bool synchronized = false; int slowSamples = 0; };
	std::map<int, State> spectators_;
	std::uint64_t startedMs_ = 0, lastSampleMs_ = 0;
	bool running_ = false;
};

}
