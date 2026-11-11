#pragma once
// The runtime's half of fighter levels: which read of the player's own levels
// is in flight, what its answer did to the value the interface reads, and when
// the next reads are due. The runtime sends the requests and decodes the
// answers; this decides, so the rules are tested without a helper
// (level_requests_test.cxx).
//
// - One read is in flight at a time. A read comes when the menu asks, once at
//   start, and twice after a set this PC fought ended (the server counts a
//   set's games a little after it): FirstReadMs and SecondReadMs later.
// - Answers belong to one service and one Ember ID. A change of either forgets
//   the value and retires the read; the counts carry on.
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
	// The helper bounds its own bridge requests; this only ends a wait for one it lost.
	static constexpr std::uint64_t GiveUpMs = 60000;
	// Reads after a set this PC fought ended, counted from when it did.
	static constexpr std::uint64_t FirstReadMs = 8000, SecondReadMs = 20000;
	// The helper's code for a bridge that has no levels.
	static constexpr const char* Unavailable = "levels_unavailable";

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
		flight_ = {};
	}

	bool MayRead() const { return !flight_.id; }

	// A request handed to the helper as `id`; 0 when it could not be, which ends it now.
	void Sent(std::uint64_t id, std::uint64_t nowMs) {
		if (!id) { Finish("helper_unavailable", std::nullopt, nowMs); return; }
		flight_ = {id, nowMs};
		answered_.loading = true;
		answered_.error.clear();
	}

	bool Awaiting(std::uint64_t id) const { return id && id == flight_.id; }

	// The answer: the value, or why there is none (the helper's code, or
	// "bridge_invalid_response"). A bridge without levels answers with an
	// Own of kind Unavailable and `Unavailable` as the code.
	void Finish(const std::string& failure, std::optional<Own> own, std::uint64_t nowMs) {
		if (own) {
			answered_.value = std::move(*own);
			answered_.loaded = true;
			answered_.installedMs = nowMs;
			++answered_.revision;
		}
		flight_ = {};
		answered_.loading = false;
		answered_.error = failure;
		++answered_.finished;
	}

	// Ends the read the helper never answered.
	void Expire(std::uint64_t nowMs) {
		if (flight_.id && nowMs - flight_.sentMs >= GiveUpMs) Finish("unavailable", std::nullopt, nowMs);
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
		if (flight_.id) return false;
		const auto spent = std::remove_if(due_.begin(), due_.end(), [&](std::uint64_t at) { return at <= nowMs; });
		const bool any = spent != due_.end();
		due_.erase(spent, due_.end());
		return any;
	}

private:
	struct InFlight { std::uint64_t id = 0, sentMs = 0; };
	std::string bridge_, emberId_;
	Answered<Own> answered_;
	InFlight flight_;
	std::vector<std::uint64_t> due_;
	std::uint64_t epoch_ = 0;
	std::vector<std::uint64_t> seen_;
};

} } }
