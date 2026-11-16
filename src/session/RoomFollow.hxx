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
// Every client chooses the same one. A stream PC's follower (RoomFollow::Start)
// takes table 1's game first, and with no game anywhere goes back to wait there.
inline int NextFollowedGame(const Snapshot& room, int watched, bool fromTableOne = false) {
	if (room.localTerminalPending) return -1;
	const auto takes = [&](int i) {
		const Table& table = room.tables[i];
		return i != watched && !room.terminalPending[i] && table.spectators.size() + table.watchingNext.size() < MaxSpectators;
	};
	for (int i = 0; i < static_cast<int>(TablesInPlay(room)); ++i)
		if (takes(i) && room.tables[i].phase == TablePhase::Playing) return i;
	return fromTableOne && takes(0) ? 0 : -1;
}

// What the member's watching asks of the room now: a Watch of `table` (a
// move), an Unwatch of it (Stop watching, until the room shows it), or nothing
// (table -1).
struct FollowStep {
	ActionKind kind = ActionKind::Watch;
	int table = -1;
	explicit operator bool() const { return table >= 0; }
};
// What this PC can send now: whether the room takes an action, and whether a
// watched game still shows here. A move waits for both; an Unwatch only for
// the room, since it goes mid-game.
struct FollowGate {
	bool roomTakesActions = false;
	bool showingWatchedGame = false;
};

// One member's watching intent, from the Watch and Unwatch it sends, its own
// or following's (Watched, StoppedWatching), reconciled with each room
// snapshot as it comes:
// - Free: what the room shows is what the member chose; nothing is sent.
// - Follow: following the room, moving by itself to the next game.
// - Nothing: the member stopped watching. Until the room shows it watching
//   nothing, an Unwatch goes for whatever table the room shows it at, again
//   after FollowMoveTimeoutMs, since a move still on its way may land after
//   the member's own Unwatch, and an Unwatch may be refused or not go at all.
//   Only a later Watch of the member's own, a place, or leaving the room ends it.
class RoomFollow {
public:
	bool On() const { return want_ == Want::Follow; }
	// Following starts for the room's local member. A stream PC follows
	// `fromTableOne`: watching nothing, it goes to a game, or else waits at
	// table 1 (NextFollowedGame).
	void Start(const Snapshot& room, bool fromTableOne = false) { *this = RoomFollow(); Own(room); want_ = Want::Follow; fromTableOne_ = fromTableOne; }
	// Following stops; watching stays where it is. Stop watching is not undone.
	void Stop() { if (want_ == Want::Follow) *this = RoomFollow(); }
	// A Watch of `table` went to the room at `nowMs`. Following's, or the
	// member's while following: the move awaited, until the room shows it or
	// FollowMoveTimeoutMs passes. The member's own otherwise: its last word.
	void Watched(int table, std::uint64_t nowMs) {
		if (want_ == Want::Follow) move_ = {table, nowMs};
		else *this = RoomFollow();
	}
	// An Unwatch of `table` went to the room at `nowMs`, or was tried: the
	// member stopped watching, and following ends at once. Pressed again, or
	// sent again here, it only restarts the wait for the room to show it.
	void StoppedWatching(const Snapshot& room, int table, std::uint64_t nowMs) {
		*this = RoomFollow(); Own(room); want_ = Want::Nothing; unwatch_ = {table, nowMs};
	}
	// The table followed now: the one a move is on its way to, else the one
	// watched; -1 when not following.
	int Followed(const Snapshot& room) const {
		return !On() ? -1 : move_.table >= 0 ? move_.table : WatchedTable(room, member_);
	}

	// What to send now. The intent ends, back to Free, once the member sits
	// down, queues or leaves the room, or in a tournament's room, which seats
	// and places everyone itself.
	FollowStep Next(const Snapshot& room, std::uint64_t nowMs, FollowGate gate) {
		if (want_ == Want::Free) return {};
		if (room.roomEpoch != epoch_ || room.localMember != member_ || room.closed || room.tournament.Active() ||
			!FindMember(room, member_) || PlaceOf(room, member_).kind != Place::Kind::None) { *this = RoomFollow(); return {}; }
		return want_ == Want::Nothing ? Unwatching(room, nowMs, gate) : Following(room, nowMs, gate);
	}

private:
	enum class Want : std::uint8_t { Free, Follow, Nothing };
	struct Sent { int table = -1; std::uint64_t sentMs = 0; };

	void Own(const Snapshot& room) { epoch_ = room.roomEpoch; member_ = room.localMember; }
	// Stop watching, until the room shows the member watching nothing.
	FollowStep Unwatching(const Snapshot& room, std::uint64_t nowMs, FollowGate gate) {
		const int watched = WatchedTable(room, member_);
		if (watched < 0) { *this = RoomFollow(); return {}; }
		if (watched == unwatch_.table && Since(unwatch_.sentMs, nowMs) < FollowMoveTimeoutMs) return {};
		return gate.roomTakesActions ? FollowStep{ActionKind::Unwatch, watched} : FollowStep{};
	}
	// Following ends once the member stops watching or locks in: a lock-in
	// that lands after following started (the press went out on an older
	// snapshot) ends it, so following never holds a start.
	FollowStep Following(const Snapshot& room, std::uint64_t nowMs, FollowGate gate) {
		// Before any wait for a move on its way, which would leave Release
		// lock-in hidden behind following until the move showed.
		if (FindMember(room, member_)->spectatorLocked) { *this = RoomFollow(); return {}; }
		const int watched = WatchedTable(room, member_);
		// A refused or lost move is not marked: what the room refuses for shows in
		// the snapshot (a full table, a game over, a game still closing), and the
		// choice below reads it again.
		if (move_.table >= 0) {
			if (watched != move_.table && Since(move_.sentMs, nowMs) < FollowMoveTimeoutMs) return {};
			move_ = {};
		}
		if (watched < 0) {
			if (!fromTableOne_) { *this = RoomFollow(); return {}; }
			return gate.roomTakesActions ? FollowStep{ActionKind::Watch, NextFollowedGame(room, -1, true)} : FollowStep{};
		}
		if (Holds(watched, room.tables[watched], nowMs) || !gate.roomTakesActions || gate.showingWatchedGame) return {};
		return {ActionKind::Watch, NextFollowedGame(room, watched, fromTableOne_)};
	}
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

	// The table watched: whether the follower has been shown a game there and
	// that game's pair, and since when no game has been under way there.
	struct Stay {
		int table = -1;
		bool watchedGame = false;
		MemberId pair[2] = {};
		bool quiet = false;
		std::uint64_t quietSinceMs = 0;
	};
	Want want_ = Want::Free;
	bool fromTableOne_ = false;
	// Whose intent it is: the room and member it was formed in.
	std::uint64_t epoch_ = 0;
	MemberId member_ = 0;
	// Following: the move on its way, and the table watched.
	Sent move_;
	Stay stay_;
	// Stop watching: the last Unwatch sent or tried.
	Sent unwatch_;
};

} }
