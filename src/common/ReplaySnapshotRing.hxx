#pragma once

// The replay's snapshots and the memento guard's ownership table
// (MementoGuards.hxx: PayloadOwners, 4096 slots, half usable). Every snapshot
// claims one payload for each key the battle holds, about 90 in a fight, so
// the number of snapshots cannot be fixed: 24 of them hold more than the table
// does. The ring is judged by the claims its snapshots hold, with room kept for
// the next one and for whatever else owns payloads. ReplaySnapshotRingTest
// covers it.

#include <cstddef>

namespace sf4e { namespace replaysnaps {

// Claims the ring may hold: three quarters of the table's usable half.
constexpr std::size_t kClaimBudget = 1536;
// The most snapshots kept, whatever their claims.
constexpr std::size_t kMostSnaps = 24;

// Whether the ring thins before it takes another snapshot: it has the most
// snapshots, or the next one (as large as the largest so far, and one more in
// case it is larger) would pass the budget. One snapshot is never thinned.
inline bool NeedsThinning(std::size_t count, std::size_t held, std::size_t largest) {
	return count >= 2 && (count >= kMostSnaps || held + 2 * largest > kClaimBudget);
}

} }
