#pragma once
// A tournament set's games in this PC's archive (docs/design/TOURNAMENT_DESK.md,
// 4b): the next one to send to the set's bridge, the sets for Replays › Sets,
// with the sets played in a room. ReplaySetPack.hxx splits a set's pack.
#include "ReplayFiles.hxx"
#include "ReplaySender.hxx"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace sf4e { namespace platform { namespace replays {

// Only a game its note says this PC played is ever sent: one it watched, or
// one without a note yet (Unknown), never is.
inline bool PlayedHere(const ArchivedReplay& replay) { return replay.provenance == replayfiles::Provenance::Played; }

// Where a replay goes: its set's bridge while it is a set's game that bridge
// has not refused for a closed set; otherwise the replay archive, under the
// player's sharing like any game outside a set.
inline bool ForItsSet(const ArchivedReplay& replay) { return replay.set.Tagged() && !replay.setClosed; }

// The oldest replay of a set this PC played that its bridge may not have yet,
// and `sending` has not put aside at `nowMs`, or null. `archive` is a
// listing, newest first.
inline const ArchivedReplay* NextToSend(const std::vector<ArchivedReplay>& archive, const ReplaySender& sending = {}, std::uint64_t nowMs = 0) {
	for (auto at = archive.rbegin(); at != archive.rend(); ++at)
		if (ForItsSet(*at) && PlayedHere(*at) && !at->sent && !sending.Aside(at->path, nowMs, true)) return &*at;
	return nullptr;
}

// The oldest game this PC played that goes to the replay archive, saved at
// or after `since` (unix seconds; 0 while sharing is off, which sends none),
// that the archive may not have yet and `sending` has not put aside at
// `nowMs`, or null. Only Ember's own files: the archive takes no other.
inline const ArchivedReplay* NextToArchive(const std::vector<ArchivedReplay>& archive, std::uint64_t since, const ReplaySender& sending = {}, std::uint64_t nowMs = 0) {
	if (!since) return nullptr;
	for (auto at = archive.rbegin(); at != archive.rend(); ++at)
		if (!ForItsSet(*at) && PlayedHere(*at) && !at->sent && at->time >= since && at->path.extension() == L".emberreplay" && !sending.Aside(at->path, nowMs)) return &*at;
	return nullptr;
}

// What `sending` sends next at `nowMs`: a set's game first, one it put
// aside waiting, then, while some bridge of the player's keeps an archive,
// any other game sharing covers.
inline const ArchivedReplay* NextReplay(const std::vector<ArchivedReplay>& archive, std::uint64_t since, const ReplaySender& sending, std::uint64_t nowMs) {
	if (const auto* set = NextToSend(archive, sending, nowMs)) return set;
	return sending.Archives() ? NextToArchive(archive, since, sending, nowMs) : nullptr;
}

// One set: its games oldest first, and how many rooms they were played in.
// A tournament set has its match ID; a set imported from a pack has no
// bridge; a set played in a room has neither, and is known by its last game.
struct ArchivedSet {
	std::string bridgeId, matchId;
	std::vector<const ArchivedReplay*> games;
	std::size_t rooms = 0;
	bool Tagged() const { return !matchId.empty(); }
};

// Two games are of one room set when the same two named players played
// them within half an hour of each other, either side.
constexpr std::uint64_t kSetGapSeconds = 1800;
inline bool SameSet(const ArchivedReplay& newer, const ArchivedReplay& older) {
	if (older.names[0].empty() || older.names[1].empty() || newer.time < older.time || newer.time - older.time > kSetGapSeconds) return false;
	return (older.names[0] == newer.names[0] && older.names[1] == newer.names[1]) || (older.names[0] == newer.names[1] && older.names[1] == newer.names[0]);
}

// The listing's sets, the set played last first: the tournament sets by
// their match ID, and the room sets of two or more games in a row
// (SameSet) among the other games. The games point into `archive`, newest
// first, which must outlive them.
inline std::vector<ArchivedSet> Sets(const std::vector<ArchivedReplay>& archive) {
	std::vector<ArchivedSet> sets;
	std::unordered_map<std::string, std::size_t> tagged;
	const ArchivedReplay* last = nullptr;
	for (const auto& replay : archive) {
		if (replay.set.matchId.empty()) {
			if (!last || !SameSet(*last, replay)) sets.push_back({"", "", {}, 0});
			sets.back().games.push_back(&replay);
			last = &replay;
			continue;
		}
		last = nullptr;
		const auto found = tagged.emplace(replay.set.matchId, sets.size());
		if (found.second) sets.push_back({replay.set.bridgeId, replay.set.matchId, {}, 0});
		sets[found.first->second].games.push_back(&replay);
	}
	sets.erase(std::remove_if(sets.begin(), sets.end(), [](const ArchivedSet& set) { return !set.Tagged() && set.games.size() < 2; }), sets.end());
	for (auto& set : sets) {
		// Appended newest first, as the listing runs.
		std::reverse(set.games.begin(), set.games.end());
		std::vector<std::uint64_t> revisions;
		for (const auto* game : set.games) revisions.push_back(game->set.revision);
		std::sort(revisions.begin(), revisions.end());
		set.rooms = set.Tagged() ? static_cast<std::size_t>(std::unique(revisions.begin(), revisions.end()) - revisions.begin()) : 0;
	}
	std::stable_sort(sets.begin(), sets.end(), [](const ArchivedSet& a, const ArchivedSet& b) { return a.games.back()->time > b.games.back()->time; });
	return sets;
}
} } }
