#pragma once
// Follow the room: a spectator's choice to move by itself to the room's next
// game whenever the table it watches stops playing. It lasts while the member
// stays in the room with no seat or queue place, and it is this client's
// alone: each move is the room's ordinary Watch, which already ends watching
// elsewhere (RoomAuthority::StopWatching), so the room host and the protocol
// know nothing of it, and a move the room refuses leaves the follower watching
// where it was. Following never locks in, so it never holds a start.
#include "RoomQueries.hxx"
#include <cstdint>
#include <limits>

namespace sf4e { namespace room {

// After a game the follower watched, how long its table keeps it while the
// same pair stays seated: an Arcade winner's next set starts at the same table.
constexpr std::uint64_t FollowRematchGraceMs = 3000;
// How long a table keeps a follower that came for its next game, while both
// seats are filled. A follower that arrives mid-game waits for the next one in
// watchingNext; without this it would move on as that game ended, and could go
// from table to table without ever being shown a game.
constexpr std::uint64_t FollowNextGameMs = 20000;
// How long a move waits for the room to show it before following decides again.
constexpr std::uint64_t FollowMoveTimeoutMs = 5000;

// The table `member` watches by choice, or -1.
inline int WatchedTable(const Snapshot& room, MemberId member) {
	for (std::size_t i = 0; i < TablesInPlay(room); ++i)
		if (WatchesByChoice(room.tables[i], member)) return static_cast<int>(i);
	return -1;
}

// The room's next game for a follower watching `watched`: the lowest-numbered
// other table in play whose game is Playing, that takes another spectator and
// whose last game is not still closing (the Watch the room would refuse).
// Every client chooses the same one.
inline int NextFollowedGame(const Snapshot& room, int watched) {
	if (room.localTerminalPending) return -1;
	for (std::size_t i = 0; i < TablesInPlay(room); ++i) {
		const Table& table = room.tables[i];
		if (static_cast<int>(i) != watched && table.phase == TablePhase::Playing && !room.terminalPending[i] &&
			table.spectators.size() + table.watchingNext.size() < MaxSpectators) return static_cast<int>(i);
	}
	return -1;
}

// One member's following, decided afresh from each room snapshot.
class RoomFollow {
public:
	bool On() const { return on_; }
	// Following starts for the room's local member.
	void Start(const Snapshot& room) { *this = RoomFollow(); on_ = true; epoch_ = room.roomEpoch; member_ = room.localMember; }
	void Stop() { *this = RoomFollow(); }
	// A Watch of `table` went to the room at `nowMs`. Until the room shows it,
	// or FollowMoveTimeoutMs passes, no other move is decided.
	void Moved(int table, std::uint64_t nowMs) { move_ = {table, nowMs}; }
	// The table followed now: the one a move is on its way to, else the one
	// watched; -1 when neither.
	int Followed(const Snapshot& room) const { return move_.table >= 0 ? move_.table : WatchedTable(room, member_); }

	// The table to Watch now, or -1. `canMove`: the room takes a Watch now and
	// no watched game is still showing on this PC. Following ends here once
	// the member sits down, queues, stops watching or leaves the room.
	int Next(const Snapshot& room, std::uint64_t nowMs, bool canMove) {
		if (!on_) return -1;
		if (room.roomEpoch != epoch_ || room.localMember != member_ || room.closed || room.tournament.Active() ||
			!FindMember(room, member_) || PlaceOf(room, member_).kind != Place::Kind::None) { Stop(); return -1; }
		const int watched = WatchedTable(room, member_);
		// A refused or lost move is not marked: what the room refuses for shows in
		// the snapshot (a full table, a game over, a game still closing), and the
		// choice below reads it again.
		if (move_.table >= 0) {
			if (watched != move_.table && Since(move_.sentMs, nowMs) < FollowMoveTimeoutMs) return -1;
			move_ = {};
		}
		if (watched < 0) { Stop(); return -1; }
		if (Holds(watched, room.tables[watched], nowMs) || !canMove) return -1;
		return NextFollowedGame(room, watched);
	}

private:
	// Whether the watched table keeps the follower: a game is under way or
	// about to start there, or the next one is still likely soon.
	bool Holds(int index, const Table& table, std::uint64_t nowMs) {
		if (stay_.table != index) stay_ = {index};
		if (table.phase == TablePhase::Ready || table.phase == TablePhase::Playing) {
			stay_.quiet = false;
			// A game it is shown, rather than one it waits out in watchingNext.
			if (table.phase == TablePhase::Playing &&
				std::find(table.spectators.begin(), table.spectators.end(), member_) != table.spectators.end()) {
				stay_.watchedGame = true; stay_.pair[0] = table.p1; stay_.pair[1] = table.p2;
			}
			return true;
		}
		if (!stay_.quiet) { stay_.quiet = true; stay_.quietSinceMs = nowMs; }
		const auto quietMs = Since(stay_.quietSinceMs, nowMs);
		if (stay_.watchedGame) return table.p1 == stay_.pair[0] && table.p2 == stay_.pair[1] && quietMs < FollowRematchGraceMs;
		return table.p1 && table.p2 && quietMs < FollowNextGameMs;
	}

	// Milliseconds from `since` to `nowMs`. A clock that went back (the shell's
	// restarts with Ember's renderer) counts as long past, so no wait outlives it.
	static std::uint64_t Since(std::uint64_t since, std::uint64_t nowMs) {
		return nowMs >= since ? nowMs - since : (std::numeric_limits<std::uint64_t>::max)();
	}

	struct Move { int table = -1; std::uint64_t sentMs = 0; };
	// The table watched: whether the follower has been shown a game there and
	// that game's pair, and since when no game has been under way there.
	struct Stay {
		int table = -1;
		bool watchedGame = false;
		MemberId pair[2] = {};
		bool quiet = false;
		std::uint64_t quietSinceMs = 0;
	};
	bool on_ = false;
	std::uint64_t epoch_ = 0;
	MemberId member_ = 0;
	Move move_;
	Stay stay_;
};

} }
