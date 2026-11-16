#pragma once

// The replay's snapshots and the memento guard's ownership table
// (MementoGuards.hxx: PayloadOwners, 4096 slots, half usable). Every snapshot
// claims one payload for each key the battle holds, about 90 in a fight, so
// the number of snapshots cannot be fixed: 24 of them hold more than the table
// does (the log's failure at frame 3096 was the 24th snapshot, at 86 to 89
// claims each; at 90 it is the 23rd). The ring is judged by the claims its
// snapshots hold, with room kept for the next one. ReplaySnapshotRingTest
// covers it with the playback's own code.
//
// A snapshot type derives from Entry and says how many claims it holds and
// whether it can still be loaded (a state whose claim was taken away cannot).
// The ring never frees a snapshot itself: each call that drops some takes the
// release the owner of the engine state needs.

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace sf4e { namespace replaysnaps {

// Claims the ring may hold: three quarters of the table's usable half.
constexpr std::size_t kClaimBudget = 1536;
// The most snapshots kept, whatever their claims.
constexpr std::size_t kMostSnaps = 24;
// Frames between snapshots at first; each thinning doubles it.
constexpr int kFirstEvery = 120;

struct Entry { int round = 0, frame = 0; };

template <class Snap>
class Ring {
public:
	// The latest usable snapshot of the round at or before the frame.
	Snap* Best(int round, int frame) const {
		Snap* best = nullptr;
		for (const auto& snap : snaps_) if (snap->round == round && snap->frame <= frame && snap->Usable() && (!best || snap->frame > best->frame)) best = snap.get();
		return best;
	}
	// The earliest usable snapshot of the round: the furthest back a jump can go.
	Snap* Earliest(int round) const {
		Snap* first = nullptr;
		for (const auto& snap : snaps_) if (snap->round == round && snap->Usable() && (!first || snap->frame < first->frame)) first = snap.get();
		return first;
	}
	// One is due when the round has none at or before this frame, then one each Every() frames after the last.
	bool Due(int round, int frame) const {
		const Snap* before = Best(round, frame);
		return !before || frame - before->frame >= every_;
	}
	// Whether the next one would not fit: the most snapshots are held, or the claims held plus two of the
	// largest (the next, and one more in case it is larger) pass the budget. One is never thinned.
	bool NeedsThinning() const {
		std::size_t held = 0, largest = 0;
		for (const auto& snap : snaps_) { held += snap->Claims(); largest = (std::max)(largest, snap->Claims()); }
		return snaps_.size() >= 2 && (snaps_.size() >= kMostSnaps || held + 2 * largest > kClaimBudget);
	}
	// Drops the snapshots that can no longer be loaded, then every other one of each round (its first kept, so a
	// jump back reaches as far as it did) and doubles the spacing.
	template <class Release>
	void Thin(Release release) {
		DropUnusable(release);
		std::vector<std::unique_ptr<Snap>> kept;
		std::vector<int> rounds, counts;
		for (auto& snap : snaps_) {
			std::size_t at = 0;
			while (at < rounds.size() && rounds[at] != snap->round) ++at;
			if (at == rounds.size()) { rounds.push_back(snap->round); counts.push_back(0); }
			if (counts[at]++ % 2 == 0) kept.push_back(std::move(snap));
			else release(*snap);
		}
		snaps_ = std::move(kept);
		every_ *= 2;
	}
	template <class Release>
	void DropUnusable(Release release) {
		Drop([](const Snap& snap) { return !snap.Usable(); }, release);
	}
	// A new round: the earlier rounds' snapshots can never be loaded, and the spacing starts over.
	template <class Release>
	void DropOtherRounds(int round, Release release) {
		const std::size_t before = snaps_.size();
		Drop([round](const Snap& snap) { return snap.round != round; }, release);
		if (snaps_.size() != before) every_ = kFirstEvery;
	}
	template <class Release>
	void Clear(Release release) {
		Drop([](const Snap&) { return true; }, release);
		every_ = kFirstEvery;
	}
	void Add(std::unique_ptr<Snap> snap) { snaps_.push_back(std::move(snap)); }
	std::size_t Size() const { return snaps_.size(); }
	bool Empty() const { return snaps_.empty(); }
	int Every() const { return every_; }
	// Each snapshot, in the order taken.
	const std::vector<std::unique_ptr<Snap>>& All() const { return snaps_; }

private:
	template <class Pick, class Release>
	void Drop(Pick pick, Release release) {
		std::vector<std::unique_ptr<Snap>> kept;
		for (auto& snap : snaps_) {
			if (pick(*snap)) release(*snap);
			else kept.push_back(std::move(snap));
		}
		snaps_ = std::move(kept);
	}
	std::vector<std::unique_ptr<Snap>> snaps_;
	int every_ = kFirstEvery;
};

} }
