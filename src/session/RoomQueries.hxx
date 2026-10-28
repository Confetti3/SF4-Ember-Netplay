#pragma once
// Queries on the room model's values that the UI and the session layer share.
#include "RoomTypes.hxx"

namespace sf4e { namespace room {

// A seated fighter may still change fighter and delay: no game is being
// prepared, played or resolved at the table, and they have not readied. An
// empty opposite seat (Idle) locks nothing.
inline bool SeatEditable(const Table& table, int seat) {
	return (table.phase == TablePhase::Idle || table.phase == TablePhase::Waiting) && !table.ready[seat];
}

// The bridge's permit for a bound table's reserved game has not reached both
// fighters yet.
inline bool PermitPending(const Table& table) {
	return table.permitGeneration != 0 &&
		(table.permits[0].empty() || table.permits[0] != table.permits[1]);
}

// A fighter who readied can still take it back: while the other seat is not
// ready, or while the start is held for a locked-in spectator or a permit.
inline bool ReadyCancellable(const Table& table, int seat) {
	return table.ready[seat] && (table.phase == TablePhase::Waiting ||
		(table.phase == TablePhase::Ready && (table.spectatorHold || PermitPending(table))));
}

// Watching this table by choice. A queued member is also listed as a
// spectator of the game it waits out, and stays listed until that game ends
// after it leaves the queue, but it did not choose to watch.
inline bool WatchesByChoice(const Table& table, MemberId member) {
	const auto listed = [member](const std::vector<MemberId>& list) {
		return std::find(list.begin(), list.end(), member) != list.end();
	};
	return (listed(table.spectators) || listed(table.watchingNext)) && !listed(table.queue) &&
		!listed(table.endingWatchers);
}

// A named seat (0 or 1) that a Queue for exactly that seat would take now: the
// table is between games, nobody is queued ahead and the seat is empty.
inline bool SeatOpenNow(const Table& table, int seat) {
	return (seat == 0 || seat == 1) && table.phase != TablePhase::Playing && table.phase != TablePhase::Paused &&
		table.queue.empty() && !(seat == 0 ? table.p1 : table.p2);
}

// What holds a fighter to the table while a game is live or being resolved.
// UnreadyFirst: the fighter's own Ready is the only thing in the way, and taking
// it back frees them.
enum class SeatHold : std::uint8_t { None, UnreadyFirst, Unresolved, AwaitingResult, InProgress };

// The hold a live table puts on its seats: an unresolved result, a finished
// game waiting for its result, or a game under way.
inline SeatHold LiveGameHold(const Table& table, bool localPostMatch) {
	if (table.phase == TablePhase::Paused) return SeatHold::Unresolved;
	if (table.phase == TablePhase::Playing && (table.resultPending || localPostMatch)) return SeatHold::AwaitingResult;
	return SeatHold::InProgress;
}

// A game is under way at the table: Ready, Playing or Paused.
inline bool GameUnderWay(const Table& table) {
	return table.phase == TablePhase::Ready || table.phase == TablePhase::Playing || table.phase == TablePhase::Paused;
}

// Why a fighter may not change fighter or appearance now. The table is
// active while a game is under way (GameUnderWay); a readied fighter has to take
// Ready back whatever the phase.
inline SeatHold ChangingFighterHold(const Table& table, int seat, bool localPostMatch) {
	if (ReadyCancellable(table, seat)) return SeatHold::UnreadyFirst;
	const bool active = GameUnderWay(table);
	return active ? LiveGameHold(table, localPostMatch) : SeatHold::None;
}

// Why a fighter may not leave the seat now. A seat is held while the table is
// active, or while this client's own game (localGameLive: preparing or playing)
// is still under way after the table went back to Waiting.
inline SeatHold LeavingSeatHold(const Table& table, int seat, bool localGameLive, bool localPostMatch) {
	const bool active = GameUnderWay(table);
	if (!active && !localGameLive) return SeatHold::None;
	if (ReadyCancellable(table, seat)) return SeatHold::UnreadyFirst;
	return LiveGameHold(table, localPostMatch);
}

// What a fighter gives up by leaving the seat: the set score, and the seat
// itself to the next player queued.
struct LeavingCost {
	bool score = false;
	bool handsOver = false;
	explicit operator bool() const { return score || handsOver; }
};
inline LeavingCost CostOfLeavingSeat(const Table& table) {
	return {table.score[0] != 0 || table.score[1] != 0, !table.queue.empty()};
}

// A live game with no result for this long is offered to the host as stuck. The
// room has no clock the clients share, so each client times it from when it
// first saw that game.
constexpr unsigned StaleGameSeconds = 600;

// Both fighters play at the higher of their Ready delays. A fighter's delay
// decides how much rollback the other side sees, so separate values gave the
// lower-delay fighter an advantage. inputDelay keeps each fighter's own choice.
inline int MatchDelay(int first, int second) { return first > second ? first : second; }
inline std::uint8_t MatchDelay(const Table& table) {
	return static_cast<std::uint8_t>(MatchDelay(table.inputDelay[0], table.inputDelay[1]));
}
// Lower input lag changes how the battle plays, so every PC that plays or
// watches a match must agree on it: it is on only when both fighters chose it.
inline bool MatchLowerLag(const Table& table) { return table.lowerLag[0] && table.lowerLag[1]; }
// A connection check names the seated pair at the table revision this PC's
// room view shows. The helper reserves it only against its committed copy of
// the room, so the check waits until that copy seats the same pair at the same
// revision (`committed` is the local server's view of the same table).
inline bool ProbePairCommitted(const Table& seen, const Table& committed) {
	return seen.p1 && seen.p2 && seen.revision == committed.revision &&
		seen.p1 == committed.p1 && seen.p2 == committed.p2;
}

// Only a player creates a Practice room, so it is never public (server
// owned) or a tournament's. Every snapshot read from the wire or a
// checkpoint holds to it, as the actions that make the other kinds do.
inline bool PracticeKindValid(const Snapshot& room) {
	return !room.practice || (!room.serverOwned && !room.tournament.Active());
}
// An Arcade room, public or private, is never a tournament's and seats from no
// queue.
inline bool ArcadeKindValid(const Snapshot& room) {
	return !room.arcade || (room.arcade <= KingRoom && !room.tournament.Active() &&
		std::all_of(room.tables.begin(), room.tables.end(), [](const Table& table) { return table.queue.empty(); }));
}

// The round time a table's games are played at: its rules', or in a Practice
// room the longest, since its games end only when the time runs out.
inline std::uint16_t MatchRoundTime(const Snapshot& room, const Table& table) {
	return room.practice ? PracticeRoundTime : table.rules.roundTime;
}

// The input delay `seat` of `table` plays its game at, from the delays captured
// at Ready: the higher of the two in a Shared room, its own in an Own one.
inline std::uint8_t SeatMatchDelay(const Snapshot& room, const Table& table, int seat) {
	return room.delayPolicy == DelayPolicy::Own ? table.inputDelay[seat == 0 ? 0 : 1] : MatchDelay(table);
}
// The delay a spectator's HUD shows for the game it watches, from the seat
// delays that game's grant carries (SeatMatchDelay): the Shared one, or Own
// when each fighter plays at their own. Taken as the stream starts and kept
// for all of it, since the table's delays belong to the next game as soon as
// its fighters ready again.
struct GameDelay { int frames = -1; bool own = false; };
inline GameDelay WatchedGameDelay(DelayPolicy policy, std::uint8_t first, std::uint8_t second) {
	return {MatchDelay(first, second), policy == DelayPolicy::Own};
}

inline const Member* FindMember(const Snapshot& snapshot, MemberId id) {
	const auto found = std::find_if(snapshot.members.begin(), snapshot.members.end(),
		[id](const Member& member) { return member.id == id; });
	return found == snapshot.members.end() ? nullptr : &*found;
}

// A member the room's tournament binding names as a watcher: it only watches,
// with no table action, host power or chat.
inline bool IsWatcher(const Snapshot& snapshot, MemberId id) {
	const auto* member = FindMember(snapshot, id);
	return member && snapshot.tournament.WatcherOf(member->connection.user);
}
// The member with the host's powers: the host field's holder, or none (0) when
// that is a watcher, which a bound room can leave holding the field.
inline MemberId ActingHost(const Snapshot& snapshot) { return IsWatcher(snapshot, snapshot.host) ? 0 : snapshot.host; }
inline std::size_t WatcherCount(const Snapshot& snapshot) {
	return static_cast<std::size_t>(std::count_if(snapshot.members.begin(), snapshot.members.end(),
		[&](const Member& member) { return snapshot.tournament.WatcherOf(member.connection.user); }));
}

// Where a member holds a place in the room: a seat at one table, a place in one
// table's queue, or neither. Watching is not a place, because Queue and Watch
// both end it.
struct Place {
	enum class Kind : std::uint8_t { None, Seat, Queue } kind = Kind::None;
	int table = -1;
	int seat = -1;
	// In a queue: the place in it, from 1 for the member who plays next.
	int position = 0;
};
// The tables a room's members can sit at, watch or set rules for; the others
// stay empty.
inline std::size_t TablesInPlay(const Snapshot& room) { return room.arcade ? TableCount : ClassicTableCount; }

// The cabinet `member` sits at in an Arcade room, or -1.
inline int CabinetOf(const Snapshot& room, MemberId member) {
	const auto at = std::find(room.cabinets.begin(), room.cabinets.end(), member);
	return member && at != room.cabinets.end() ? static_cast<int>(at - room.cabinets.begin()) : -1;
}

inline Place PlaceOf(const Snapshot& snapshot, MemberId id) {
	Place place;
	const auto* member = FindMember(snapshot, id);
	if (!member) return place;
	if (member->seat >= 0 && member->seat < 2 && member->table >= 0 && member->table < static_cast<int>(snapshot.tables.size())) {
		place.kind = Place::Kind::Seat; place.table = member->table; place.seat = member->seat;
		return place;
	}
	for (const auto& table : snapshot.tables) {
		const auto queued = std::find(table.queue.begin(), table.queue.end(), id);
		if (queued != table.queue.end()) {
			place.kind = Place::Kind::Queue; place.table = table.id;
			place.position = static_cast<int>(queued - table.queue.begin()) + 1;
			return place;
		}
	}
	return place;
}

// The member in the other seat of `table` from `local`'s point of view, or
// nullptr when `local` is not seated there or the seat is empty.
inline const Member* SeatOpponent(const Snapshot& snapshot, std::uint8_t table, MemberId local) {
	if (table >= snapshot.tables.size() || !local) return nullptr;
	const Table& at = snapshot.tables[table];
	if (at.p1 != local && at.p2 != local) return nullptr;
	const MemberId other = at.p1 == local ? at.p2 : at.p1;
	return other ? FindMember(snapshot, other) : nullptr;
}

// True when a custom room's snapshot gives this client no seat: it stood up,
// waits in a queue, or is no longer a member. A legacy lobby (no epoch) has no
// seats to lose.
inline bool LocalLeftSeat(const Snapshot& snapshot) {
	return snapshot.roomEpoch && PlaceOf(snapshot, snapshot.localMember).kind != Place::Kind::Seat;
}

} }
