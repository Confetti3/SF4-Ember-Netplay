#pragma once

// The replay files waiting to be imported, however they reached Ember: the
// Replays screen's picker, a drop on the game window or a link's Import. One
// queue and one set of rules for all three. The runtime imports from it one
// file at a time on the game thread, and only while the player is on Ember's
// menus; in a room or a match the files wait. Adding is safe on any thread.

#include "ReplayLink.hxx"
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace replay_import {

constexpr std::size_t kMostWaiting = 64;

// What a run of imports came to: the files in it, how many could not be
// imported, and how many games those that could be imported hold.
struct Batch {
	std::size_t files = 0, failed = 0, held = 0;
};

class Queue {
public:
	// UTF-8 paths. Anything that is not a replay or pack file is left out, and
	// no more than kMostWaiting wait. Returns how many were taken; a batch
	// already under way takes them into itself.
	std::size_t Add(const std::vector<std::string>& files) {
		std::lock_guard<std::mutex> lock(mutex_);
		std::size_t taken = 0;
		for (const auto& file : files) {
			if (waiting_.size() >= kMostWaiting) break;
			if (!replay_link::IsReplayPath(file)) continue;
			waiting_.push_back(file);
			++taken;
		}
		if (!taken) return 0;
		if (!open_) { open_ = true; batch_ = {}; ++batches_; }
		batch_.files += taken;
		return taken;
	}
	// Files still to import, and how many batches have begun (to tell a new one
	// from the last).
	std::size_t Waiting() const { std::lock_guard<std::mutex> lock(mutex_); return waiting_.size(); }
	std::uint64_t Batches() const { std::lock_guard<std::mutex> lock(mutex_); return batches_; }
	// The next file to import, or none.
	std::optional<std::string> Next() {
		std::lock_guard<std::mutex> lock(mutex_);
		if (waiting_.empty()) return std::nullopt;
		std::string file = std::move(waiting_.front());
		waiting_.pop_front();
		return file;
	}
	// What became of the file Next gave: how many of its games Replays holds, 0
	// when it could not be imported. The batch, once that was its last file.
	std::optional<Batch> Answered(std::size_t held) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!held) ++batch_.failed; else batch_.held += held;
		if (!waiting_.empty()) return std::nullopt;
		open_ = false;
		return batch_;
	}
private:
	mutable std::mutex mutex_;
	std::deque<std::string> waiting_;
	Batch batch_;
	bool open_ = false;
	std::uint64_t batches_ = 0;
};

} }
