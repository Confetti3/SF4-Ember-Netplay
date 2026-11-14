#pragma once
// The runtime's half of fighter levels: which read of the player's own levels
// is in flight, what its answer did to the value the interface reads, and when
// the next reads are due. The runtime sends the requests and decodes the
// answers; this decides, so the rules are tested without a helper
// (level_requests_test.cxx).
//
// - One read is in flight at a time. A read comes when the menu asks, once at
//   start, and twice after a set this PC fought ended (the server counts a
//   set's games a little after it): FirstReadMs and SecondReadMs later. The
//   runtime sends it through BridgeFlights, which routes the answer, gives up
//   on a lost one and ends one the helper could not take; the answer comes
//   here under the flight it began with.
// - Answers belong to one service and one Ember ID. A change of either forgets
//   the value and retires the read (its answer is then nobody's); the counts
//   carry on.
// - A failed read keeps the value it had.
#include "Answered.hxx"
#include "Levels.hxx"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace netplay { namespace levels {

class Requests {
public:
	// Reads after a set this PC fought ended, counted from when it did.
	static constexpr std::uint64_t FirstReadMs = 8000, SecondReadMs = 20000;
	// The helper's code for a bridge that has no levels.
	static constexpr const char* Unavailable = "points_unavailable";

	// The first read is due at once: the one at start.
	Requests() : due_(1, 0) {}

	const Answered<Own>& Published() const { return answered_; }
	const std::string& Bridge() const { return bridge_; }
	const std::string& EmberId() const { return emberId_; }

	// The service and Ember ID the next request is for.
	void Scope(const std::string& bridge, const std::string& emberId) {
		if (bridge == bridge_ && emberId == emberId_) return;
		bridge_ = bridge;
		emberId_ = emberId;
		Answered<Own> fresh;
		fresh.finished = answered_.finished;
		fresh.revision = answered_.revision;
		answered_ = std::move(fresh);
		flight_ = 0;
	}

	bool MayRead() const { return !flight_; }

	// A read is going to the helper: returns the flight its answer comes back under (Finish).
	std::uint64_t Begin() {
		flight_ = ++flights_;
		answered_.loading = true;
		answered_.error.clear();
		return flight_;
	}

	bool Awaiting(std::uint64_t flight) const { return flight && flight == flight_; }

	// The answer to `flight`: the value, or why there is none (the helper's
	// code, or "bridge_invalid_response"). A bridge without levels answers
	// with an Own of kind Unavailable and `Unavailable` as the code. The
	// answer to a retired flight changes nothing.
	void Finish(std::uint64_t flight, const std::string& failure, std::optional<Own> own, std::uint64_t nowMs) {
		if (!Awaiting(flight)) return;
		if (own) {
			answered_.value = std::move(*own);
			answered_.loaded = true;
			answered_.installedMs = nowMs;
			++answered_.revision;
		}
		flight_ = 0;
		answered_.loading = false;
		answered_.error = failure;
		++answered_.finished;
	}

	// A set this PC fought ended at `nowMs`: two reads are due after it.
	void SetEnded(std::uint64_t nowMs) { due_ = {nowMs + FirstReadMs, nowMs + SecondReadMs}; }

	// Which sets ended in the room, to schedule the reads after them.
	// `fought[table]` is the generation of the table's last set when this
	// player played in it, else 0. The first look at a room (a new `epoch`)
	// only notes what already ended.
	void NoteSets(std::uint64_t epoch, const std::vector<std::uint64_t>& fought, std::uint64_t nowMs) {
		bool ended = false;
		if (epoch == epoch_ && fought.size() == seen_.size())
			for (std::size_t table = 0; table < fought.size(); ++table) ended = ended || (fought[table] && fought[table] != seen_[table]);
		epoch_ = epoch;
		seen_ = fought;
		if (ended) SetEnded(nowMs);
	}

	// Whether a read is due at `nowMs` and none is in flight; the due ones are then spent.
	bool TakeDue(std::uint64_t nowMs) {
		if (flight_) return false;
		const auto spent = std::remove_if(due_.begin(), due_.end(), [&](std::uint64_t at) { return at <= nowMs; });
		const bool any = spent != due_.end();
		due_.erase(spent, due_.end());
		return any;
	}

private:
	std::string bridge_, emberId_;
	Answered<Own> answered_;
	// The read on its way (0: none), and how many have begun.
	std::uint64_t flight_ = 0, flights_ = 0;
	std::vector<std::uint64_t> due_;
	std::uint64_t epoch_ = 0;
	std::vector<std::uint64_t> seen_;
};

} } }
