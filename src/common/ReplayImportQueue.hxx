#pragma once

// The replay files waiting to be imported, however they reached Ember: the
// Replays screen's picker, a drop on the game window or a link's Import. One
// queue and one set of rules for all three. The runtime imports from it one
// file at a time on the game thread, and only while the player is on Ember's
// menus; in a room or a match the files wait. Adding is safe on any thread.

#include "ReplayLink.hxx"
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
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
	// How many batches have ended. A batch that begins and ends between two looks at the
	// queue shows Batches and Finished both moved; Finished >= Batches says the last is over.
	std::uint64_t Finished() const { std::lock_guard<std::mutex> lock(mutex_); return finished_; }
	// The files still waiting, in order (to keep them across a restart).
	std::vector<std::string> Pending() const { std::lock_guard<std::mutex> lock(mutex_); return std::vector<std::string>(waiting_.begin(), waiting_.end()); }
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
		++finished_;
		return batch_;
	}
private:
	mutable std::mutex mutex_;
	std::deque<std::string> waiting_;
	Batch batch_;
	bool open_ = false;
	std::uint64_t batches_ = 0, finished_ = 0;
};

// Files still waiting when the game closes are kept in a small text file, one UTF-8 path a
// line, and taken back at the next start, so a drop made during a match is not lost. The files
// stay where the player had them; one that is gone by then is reported as a failed import.
inline bool Keep(const std::filesystem::path& file, const std::vector<std::string>& waiting) {
	std::error_code ignored;
	if (waiting.empty()) { std::filesystem::remove(file, ignored); return true; }
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	for (const auto& path : waiting) out << path << '
';
	return static_cast<bool>(out.flush());
}
// Takes the kept files back into `queue` and deletes the note. Returns how many were taken.
inline std::size_t Restore(const std::filesystem::path& file, Queue& queue) {
	std::ifstream in(file, std::ios::binary);
	if (!in) return 0;
	std::vector<std::string> paths;
	std::string line;
	while (paths.size() < kMostWaiting && std::getline(in, line)) {
		if (!line.empty() && line.back() == '') line.pop_back();
		if (!line.empty()) paths.push_back(line);
	}
	in.close();
	std::error_code ignored;
	std::filesystem::remove(file, ignored);
	return queue.Add(paths);
}

// One tick of the import: only on Ember's menus with no room joined, the next file goes to
// `importFile` (UTF-8 path in, how many games it holds out, 0 when it could not be imported).
// The batch is returned on the tick its last file was answered. Files wait otherwise.
template <class Import>
std::optional<Batch> Step(Queue& queue, bool atMenu, bool noRoom, Import&& importFile) {
	if (!atMenu || !noRoom) return std::nullopt;
	const auto file = queue.Next();
	if (!file) return std::nullopt;
	return queue.Answered(importFile(*file));
}

} }
