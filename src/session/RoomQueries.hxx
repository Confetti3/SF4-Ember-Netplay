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

// Whether a member on `link` may take a seat, a queue's turn or a cabinet in
// the room. A room for wired players (Snapshot::wiredOnly) refuses Wi-Fi
// alone: an Unknown link always sits, since Linux and Steam Deck report
// nothing else (#159), and neither do VPNs or virtual adapters.
inline bool LinkMaySit(const Snapshot& room, NetworkLink link) {
	return !room.wiredOnly || link != NetworkLink::Wireless;
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

// What a room type allows: the one place each of its rules is decided. Code
// that behaves differently by room type asks RulesOf for the rule, never the
// type itself, so a type added later is a row here.
struct TypeRules {
	// Members sit at one-player cabinets (Snapshot::cabinets) and play whoever
	// has waited longest at whichever table is free: a Queue or Unqueue names a
	// cabinet, no table has a queue, whoever finds no table waits in line, and
	// a pair still together after a set chooses Rematch or New opponent.
	// Otherwise each table seats from its own queue and rotates after a set.
	bool cabinets;
	// Every table's rules are the room's, set when it is created; no SetRules.
	bool rulesFixed;
	// A set ends after at least one game, even with no set length (SetGoal).
	bool setAlwaysEnds;
	// A set's winner stays for the next challenger: cabinets count the sets
	// won in a row, and the longest streak holds the top slot.
	bool winnerStays;
	// The rules may keep a set's winner on their fighter (Rules::winnerKeepsFighter).
	bool keeperOption;
	// A tournament's binding may make the room its match's.
	bool tournamentBindable;
	// How many tables it plays at unless created with another count.
	std::uint8_t defaultTableCount;
};
// The rules of `type`. Out of range, which no snapshot or action holds
// (ArcadeKindValid, ValidRoomType), they are Arcade's, as for any type but
// Classic before there was a table.
inline const TypeRules& RulesOf(RoomType type) {
	//                                 cabinets rulesFixed setAlwaysEnds winnerStays keeperOption tournamentBindable defaultTableCount
	static const TypeRules classic = {false,   false,     false,        false,      false,       true,              static_cast<std::uint8_t>(ClassicTableCount)};
	static const TypeRules arcade =  {true,    true,      true,         false,      true,        false,             static_cast<std::uint8_t>(TableCount)};
	static const TypeRules king =    {true,    true,      true,         true,       true,        false,             static_cast<std::uint8_t>(TableCount)};
	switch (type) {
	case RoomType::Classic: return classic;
	case RoomType::Arcade: return arcade;
	case RoomType::King: return king;
	}
	return arcade;
}
inline const TypeRules& RulesOf(const Snapshot& room) { return RulesOf(room.type); }

// Only a player creates a Practice room, so it is never public (server
// owned) or a tournament's. Every snapshot read from the wire or a
// checkpoint holds to it, as the actions that make the other kinds do.
inline bool PracticeKindValid(const Snapshot& room) {
	return !room.practice || (!room.serverOwned && !room.tournament.Active());
}
// How many tables a room plays at unless it was created with another count:
// four for Classic, all eight for Arcade and King of the hill, as before
// either could choose.
inline std::uint8_t DefaultTableCount(RoomType type) { return RulesOf(type).defaultTableCount; }
// The room `room` is now, as a Charter: what a room replacing it is created as.
inline Charter CharterOf(const Snapshot& room) {
	Charter charter;
	charter.type = room.type;
	charter.practice = room.practice;
	charter.delayPolicy = room.delayPolicy;
	charter.tableCount = room.tableCount;
	charter.wiredOnly = room.wiredOnly;
	return charter;
}
// The tables a room's members can sit at, watch or set rules for; the others
// stay empty. How many is the room's (Snapshot::tableCount).
inline std::size_t TablesInPlay(const Snapshot& room) {
	return room.tableCount < 1 ? 1 : room.tableCount > TableCount ? TableCount : room.tableCount;
}
// An Arcade table where one fighter waits for an opponent, and an empty one.
inline bool ArcadeAlone(const Table& table) {
	return table.phase == TablePhase::Idle && (table.p1 == 0) != (table.p2 == 0);
}
inline bool ArcadeEmpty(const Table& table) { return table.phase == TablePhase::Idle && !table.p1 && !table.p2; }

// Whether New opponent parts the Arcade pair at `table` at once: someone
// waits in line or alone at another table, or another table is free to wait at.
// Otherwise every table is a pair and the request waits (Table::wantsNew).
inline bool ArcadeSomeoneFree(const Snapshot& room, std::size_t table) {
	if (!room.line.empty()) return true;
	for (std::size_t other = 0; other < TablesInPlay(room); ++other)
		if (other != table && (ArcadeAlone(room.tables[other]) || ArcadeEmpty(room.tables[other]))) return true;
	return false;
}

// A fighter still seated with their last opponent who asked for a new one
// while nobody else was free (Table::wantsNew).
inline bool ArcadeAsked(const Table& table, int seat) { return table.deciding && table.wantsNew[seat]; }
inline bool ArcadeAwaitsNew(const Table& table) { return ArcadeAsked(table, 0) || ArcadeAsked(table, 1); }

// A fighter waiting for a new opponent at a table other than `except`
// (TableCount: at any), and that table; member 0 when nobody waits. Only one table ever waits: the next
// request, set end or freed seat answers it.
struct ArcadeWaitingAsker { std::size_t table = 0; MemberId member = 0; };
inline ArcadeWaitingAsker ArcadeAsker(const Snapshot& room, std::size_t except) {
	for (std::size_t other = 0; other < room.tables.size(); ++other) {
		const auto& t = room.tables[other];
		if (other == except) continue;
		if (ArcadeAsked(t, 0)) return {other, t.p1};
		if (ArcadeAsked(t, 1)) return {other, t.p2};
	}
	return {};
}

// What New opponent does for the pair deciding at `table`, the one rule the
// room applies (RoomAuthority::ApplyRematch) and every view describes: part
// now when someone else is free, trade places with a fighter waiting for a new
// opponent elsewhere, or wait for the next set to end or seat to come free.
enum class ArcadeNewOpponent : std::uint8_t { PartNow, Swap, Wait };
inline ArcadeNewOpponent ArcadeNewOpponentFor(const Snapshot& room, std::size_t table) {
	if (ArcadeSomeoneFree(room, table)) return ArcadeNewOpponent::PartNow;
	return ArcadeAsker(room, table).member ? ArcadeNewOpponent::Swap : ArcadeNewOpponent::Wait;
}

// A room of a known type, bound to a tournament only if its type may be, and
// with no table queue if it seats by cabinet. Every snapshot read from the
// wire or a checkpoint holds to it.
inline bool ArcadeKindValid(const Snapshot& room) {
	if (!ValidRoomType(static_cast<int>(room.type))) return false;
	const auto& rules = RulesOf(room);
	return (rules.tournamentBindable || !room.tournament.Active()) && (!rules.cabinets ||
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

// Whether the room takes a post-match answer from `member` now, as RoomAuthority::ApplyPostMatchAnswer judges it:
// they are a fighter of the answer's table, and that table still plays the answer's match. A set's rotation that
// unseated them, or a table gone on to another match, takes no answer.
inline bool PostMatchTakes(const Snapshot& room, const Member& member, const PostMatchSaid& said) {
	if (member.table < 0 || static_cast<std::size_t>(member.table) >= room.tables.size() || static_cast<std::uint8_t>(member.table) != said.table) return false;
	const auto& table = room.tables[static_cast<std::size_t>(member.table)];
	return (table.p1 == member.id || table.p2 == member.id) && table.matchGeneration == said.generation;
}

// PostMatchDue for this PC's own member of `room`: none is due once the room no longer takes the answer
// (PostMatchTakes), so a fighter the set's rotation unseated stops telling it.
inline bool PostMatchDue(const PostMatchSaid& wanted, const PostMatchSaid& lastSaid, std::uint64_t lastSaidAtMs,
	std::uint64_t nowMs, bool live, const Snapshot& room) {
	const auto* member = room.roomEpoch ? FindMember(room, room.localMember) : nullptr;
	return member && PostMatchTakes(room, *member, wanted) && PostMatchDue(wanted, lastSaid, lastSaidAtMs, nowMs, live, member);
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
// table's queue or in an Arcade room's line (at no table), or neither. Watching
// is not a place, because Queue and Watch both end it.
struct Place {
	enum class Kind : std::uint8_t { None, Seat, Queue, Line } kind = Kind::None;
	int table = -1;
	int seat = -1;
	// In a queue or the line: the place in it, from 1 for the member who plays next.
	int position = 0;
};
// Nobody sits, queues or watches at `table` and no game of it is under way or
// waiting on its result: a Classic room's host may take it away (SetTableCount).
inline bool TableEmpty(const Table& table) {
	return !table.p1 && !table.p2 && table.queue.empty() && table.spectators.empty() && table.watchingNext.empty() &&
		table.endingWatchers.empty() && table.phase != TablePhase::Playing && table.phase != TablePhase::Paused &&
		!table.resultPending && !table.permitGeneration;
}
// Whether the room has a table count its host may change (SetTableCount): a
// private room. A public or tournament room keeps the tables it opened with.
inline bool TableCountChangeable(const Snapshot& room) {
	return !room.serverOwned && !room.tournament.Active();
}
// Whether Create offers the count for the room it is about to open: a private
// room, and a public Arcade or King of the hill one (sent with its type,
// netplay::CreatedRules).
inline bool TableCountChosen(RoomType type, bool isPublic) { return !isPublic || type != RoomType::Classic; }
// `member`'s place in an Arcade room's line, from 1 for the player seated
// next; 0 when they are not in line.
inline int ArcadeLinePlace(const Snapshot& room, MemberId member) {
	const auto at = std::find(room.line.begin(), room.line.end(), member);
	return member && at != room.line.end() ? static_cast<int>(at - room.line.begin()) + 1 : 0;
}
// Whether the host may set the room to `count` tables now: any count up to
// TableCount in a room whose count can change, as long as each table it takes
// away is empty.
inline bool TableCountAllowed(const Snapshot& room, std::size_t count) {
	if (!TableCountChangeable(room) || count < 1 || count > TableCount) return false;
	for (std::size_t i = count; i < TablesInPlay(room); ++i) if (!TableEmpty(room.tables[i])) return false;
	return true;
}

// The cabinet `member` sits at in an Arcade room, or -1.
inline int CabinetOf(const Snapshot& room, MemberId member) {
	const auto at = std::find(room.cabinets.begin(), room.cabinets.end(), member);
	return member && at != room.cabinets.end() ? static_cast<int>(at - room.cabinets.begin()) : -1;
}
// Whether anyone but `a` and `b` sits at a cabinet: someone either of them
// could still play once a set ends.
inline bool OthersAtCabinets(const Snapshot& room, MemberId a, MemberId b) {
	return std::any_of(room.cabinets.begin(), room.cabinets.end(), [&](MemberId m) { return m && m != a && m != b; });
}
// In a room that seats by cabinet, Queue and Unqueue name a cabinet, not the
// table its pair plays at: table + seat * TableCount, a table out of play too.
inline bool NamesCabinet(const Snapshot& room, ActionKind kind) {
	return RulesOf(room).cabinets && (kind == ActionKind::Queue || kind == ActionKind::Unqueue);
}
// The cabinet a Queue or Unqueue names there; its seat is 0 or 1.
inline std::size_t CabinetNamed(const Action& action) {
	return action.table + static_cast<std::size_t>(action.seat) * TableCount;
}
// The Queue that sits down at `cabinet` (below CabinetCount).
inline Action CabinetQueueAction(std::size_t cabinet) {
	Action action;
	action.kind = ActionKind::Queue;
	action.table = static_cast<std::uint8_t>(cabinet % TableCount);
	action.seat = static_cast<std::int8_t>(cabinet / TableCount);
	return action;
}

inline Place PlaceOf(const Snapshot& snapshot, MemberId id) {
	Place place;
	const auto* member = FindMember(snapshot, id);
	if (!member) return place;
	if (member->seat >= 0 && member->seat < 2 && member->table >= 0 && member->table < static_cast<int>(snapshot.tables.size())) {
		place.kind = Place::Kind::Seat; place.table = member->table; place.seat = member->seat;
		return place;
	}
	if (const int position = ArcadeLinePlace(snapshot, id)) { place.kind = Place::Kind::Line; place.position = position; return place; }
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

// In an Arcade or King of the hill room whose rules keep winners on their
// fighters (Rules::winnerKeepsFighter), whether `member` is the keeper of
// the table they sit at (Table::keeper): until they lose a set there or
// leave it, only the Ultra and the look change.
inline bool KeepsFighter(const Snapshot& room, MemberId member) {
	if (!RulesOf(room).keeperOption || !member) return false;
	const auto place = PlaceOf(room, member);
	return place.kind == Place::Kind::Seat && room.tables[place.table].keeper == member;
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

// The fighter the room board shows for a member: the last pick they shared,
// else their profile's main, which stands for them until they pick (-1:
// neither). A matchup is what the two seats show, so a first pick that is not
// the main changes it as any later change does: the authority takes back a
// Ready given against it, the client does not resend one, and the other player
// is asked to check their Ultra (OpponentFighterWatch).
inline int ShownFighter(const Member& member) {
	return member.fighter >= 0 ? member.fighter : member.mainFighter;
}

// True when a custom room's snapshot gives this client no seat: it stood up,
// waits in a queue, or is no longer a member. A legacy lobby (no epoch) has no
// seats to lose.
inline bool LocalLeftSeat(const Snapshot& snapshot) {
	return snapshot.roomEpoch && PlaceOf(snapshot, snapshot.localMember).kind != Place::Kind::Seat;
}

// The games that win a set at `table`: its format's, or one in an Arcade or King
// room with no set length (the authority's rule in RoomAuthority::RecordResult).
// 0 means the set never ends.
inline std::uint32_t SetGoal(const Snapshot& room, const Table& table) {
	return (std::max)(static_cast<std::uint32_t>(table.rules.format), RulesOf(room).setAlwaysEnds ? 1u : 0u);
}

// Whether the game that just ended with `result` leaves the pair to fight on in
// place, rather than ending the set for the room to rotate.
inline bool RematchInPlaceOffered(const Snapshot& room, const Table& table, MatchResult result) {
	const auto goal = SetGoal(room, table);
	const int seat = result == MatchResult::P1Win ? 0 : result == MatchResult::P2Win ? 1 : -1;
	return goal == 0 || seat < 0 || table.score[seat] + 1 < goal;
}

// The set as the game `generation` at `table`, which ended with `result`,
// leaves it: the set it ended, as the room recorded it (Table::lastSet); the
// room's score, once the room has the result; or, while it does not yet (a
// game held for a rematch in place, or a result still on its way), the room's
// score with this game counted. A result not known yet counts for nothing.
struct SetStanding {
	std::uint32_t score[2] = {0, 0};
	bool over = false;
};
inline SetStanding StandingAfter(const Snapshot& room, const Table& table, std::uint64_t generation, MatchResult result) {
	SetStanding standing;
	const bool ended = generation && table.lastSet.generation == generation;
	const auto& score = ended ? table.lastSet.score : table.score;
	standing.score[0] = score[0]; standing.score[1] = score[1];
	standing.over = ended;
	const bool live = table.matchGeneration == generation && (table.phase == TablePhase::Playing || table.phase == TablePhase::Paused);
	const int seat = result == MatchResult::P1Win ? 0 : result == MatchResult::P2Win ? 1 : -1;
	if (ended || !live || seat < 0) return standing;
	++standing.score[seat];
	standing.over = !RematchInPlaceOffered(room, table, result);
	return standing;
}

// The two seats of the game `generation` at `table`: the set's, once the room
// recorded the set it ended (its rotation has moved them since), else the
// table's own.
inline std::pair<MemberId, MemberId> GameSeats(const Table& table, std::uint64_t generation) {
	return table.lastSet.generation == generation ? std::make_pair(table.lastSet.p1, table.lastSet.p2) : std::make_pair(table.p1, table.p2);
}
// Whether the set's rotation keeps `member` seated (RotationKeeps). Before the
// room has the result, `result` names the winner.
inline bool RotationKeepsMember(const Table& table, std::uint64_t generation, MatchResult result, MemberId member) {
	const int winnerSeat = table.lastSet.generation == generation ? table.lastSet.winnerSeat :
		result == MatchResult::P1Win ? 0 : result == MatchResult::P2Win ? 1 : -1;
	const auto seats = GameSeats(table, generation);
	if (!member || winnerSeat < 0 || (member != seats.first && member != seats.second)) return false;
	return RotationKeeps(table.rules.rotation, member == (winnerSeat == 0 ? seats.first : seats.second));
}
// Whether `member`, who fought the game `generation` at `table`, keeps the
// seat when they give no answer after it. Only a room that seats from a
// table's queue hands a seat over after a set, and only from a player the
// rotation moves on: the one it keeps stays for the next challenger. A room
// that seats by cabinet keeps everyone at their cabinet and rotates them itself
// (RoomAuthority::ArcadeSetEnded); a set's game keeps both seats: a set one the
// room has recorded as ended, or that its result ends, is over (StandingAfter).
inline bool KeepsSeatAfterGame(const Snapshot& room, const Table& table, std::uint64_t generation, MatchResult result, MemberId member) {
	if (RulesOf(room).cabinets) return true;
	if (table.rules.format == SetFormat::Unlimited) return false;
	return !StandingAfter(room, table, generation, result).over || RotationKeepsMember(table, generation, result, member);
}

} }
