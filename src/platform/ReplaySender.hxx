#pragma once
// The one replay at a time the game has the helper send (SendSetReplays): a
// set's game to its bridge, or another game to the replay archive. A request
// that is out is never replaced. The helper gives each 45 s from when it
// receives it (tournament/replays.rs, REPLAY_BUDGET), its wait for a turn
// included, and starts no work for one past that, so it has answered or been
// cut off long before kAnswerMs. Giving up then, as a failure, and sending
// the replay again cannot overlap a live first copy; an answer that still
// comes for the one given up is ignored.
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace sf4e { namespace platform { namespace replays {

// A refusal of a send that sending it again cannot change: the bridge has
// no such set for this player, or the file is not a replay it takes. Any
// other failure is tried again later.
inline bool SendSettled(const std::string& code) {
	for (const char* settled : {"invalid_request", "invalid_replay", "not_found", "forbidden", "unsupported_rules", "corrupt", "file_unreadable", "invalid_path", "replays_unsupported"})
		if (code == settled) return true;
	return false;
}

class ReplaySender {
public:
	// The wait for an answer, then before the next look after a failure (or
	// with nothing to send) and after a replay is done with, and how long a
	// set's game its bridge has not confirmed yet is put aside.
	static constexpr std::uint64_t kAnswerMs = 180000, kAgainMs = 60000, kNextMs = 5000, kUnconfirmedMs = 600000;
	enum class Step { Wait, GaveUp, Send };
	// Whether to send the next replay at `nowMs`: Wait while one is out or
	// until the wait after the last is over. One out kAnswerMs without an
	// answer is given up (GaveUp), as a failure.
	Step Poll(std::uint64_t nowMs) {
		if (request_) {
			if (nowMs - sentMs_ < kAnswerMs) return Step::Wait;
			request_ = 0; nextMs_ = nowMs + kAgainMs;
			return Step::GaveUp;
		}
		if (nowMs < nextMs_) return Step::Wait;
		nextMs_ = nowMs + kAgainMs;
		return Step::Send;
	}
	// Helper request `id`, sent at `nowMs`, carries the replay at `path`, a
	// set's game when `set`.
	void Sent(std::uint64_t id, std::filesystem::path path, bool set, std::uint64_t nowMs) {
		request_ = id; path_ = std::move(path); set_ = set; sentMs_ = nowMs;
	}
	// What the helper's answer to request `id` settles. NotOurs for any but
	// the request out; Done when the bridge has the replay or refused it for
	// good (it is marked sent); SetClosed when the set's bridge refused it for
	// a closed set, done for the set only (it may go to the replay archive);
	// NoArchive when none of the player's bridges keeps an archive, so only
	// sets' games go until the next start; Unconfirmed when the set's bridge
	// has not confirmed that game yet (held for review, say), so it is put
	// aside for kUnconfirmedMs and the others go meanwhile. Any other
	// failure is Again: the same replay, kAgainMs later.
	enum class Answer { NotOurs, Again, Done, SetClosed, NoArchive, Unconfirmed };
	Answer Take(std::uint64_t id, bool ok, const std::string& reason, std::uint64_t nowMs) {
		if (!id || id != request_) return Answer::NotOurs;
		request_ = 0;
		if (!ok && !set_ && reason == "replays_unsupported") { noArchive_ = true; nextMs_ = nowMs + kNextMs; return Answer::NoArchive; }
		if (!ok && set_ && reason == "set_closed") { nextMs_ = nowMs + kNextMs; return Answer::SetClosed; }
		if (!ok && set_ && reason == "replay_unconfirmed") {
			for (auto at = aside_.begin(); at != aside_.end();) at = at->second <= nowMs ? aside_.erase(at) : std::next(at);
			aside_[path_] = nowMs + kUnconfirmedMs; nextMs_ = nowMs + kNextMs;
			return Answer::Unconfirmed;
		}
		if (ok || SendSettled(reason)) { nextMs_ = nowMs + kNextMs; return Answer::Done; }
		nextMs_ = nowMs + kAgainMs;
		return Answer::Again;
	}
	bool Archives() const { return !noArchive_; }
	// Whether the replay at `path` is put aside at `nowMs` (Unconfirmed).
	bool Aside(const std::filesystem::path& path, std::uint64_t nowMs) const {
		const auto found = aside_.find(path);
		return found != aside_.end() && nowMs < found->second;
	}
	const std::filesystem::path& Path() const { return path_; }
	bool Set() const { return set_; }
private:
	std::uint64_t request_ = 0, sentMs_ = 0, nextMs_ = 0;
	std::filesystem::path path_;
	bool set_ = false, noArchive_ = false;
	// Put aside until then (ms); a few at most, and the past ones dropped.
	std::map<std::filesystem::path, std::uint64_t> aside_;
};

} } }
