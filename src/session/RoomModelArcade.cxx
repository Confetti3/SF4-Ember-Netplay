// An Arcade or King of the hill room's seating (Snapshot::arcade): sixteen
// one-player cabinets, and tables that are pairs, not places. Whoever sits
// down plays the player who has waited longest, at whichever table is free.
// Other rooms never reach this code.
#include "RoomModel.hxx"

#include <algorithm>

namespace sf4e { namespace room {

namespace {
// One fighter waits at the table for an opponent.
bool Alone(const Table& table) {
	return table.phase == TablePhase::Idle && (table.p1 == 0) != (table.p2 == 0);
}
MemberId Lone(const Table& table) { return table.p1 ? table.p1 : table.p2; }
bool Empty(const Table& table) { return table.phase == TablePhase::Idle && !table.p1 && !table.p2; }
std::uint64_t NextArrival(const Snapshot& room) {
	std::uint64_t next = 1;
	for (const auto& table : room.tables) next = (std::max)(next, table.waitingSince + 1);
	return next;
}
}

// The table of the player who has waited longest for `member`, not counting
// the one they just parted from.
Table* RoomAuthority::Waiter(MemberId member, MemberId parted, const Table* except) {
	Table* longest = nullptr;
	for (auto& table : snapshot_.tables) {
		if (&table == except || !Alone(table) || Lone(table) == member) continue;
		if (parted && Lone(table) == parted) continue;
		if (!longest || table.waitingSince < longest->waitingSince) longest = &table;
	}
	return longest;
}

// `member` takes the open seat at `table`: a new pair at 0-0, or one waiting.
void RoomAuthority::JoinTable(MemberId member, Table& table) {
	if (!table.p1) table.p1 = member; else table.p2 = member;
	ClearReadiness(table);
	table.score[0] = table.score[1] = 0;
	table.deciding = false; table.rematch[0] = table.rematch[1] = false;
	const bool pair = table.p1 && table.p2;
	table.phase = pair ? TablePhase::Waiting : TablePhase::Idle;
	table.waitingSince = pair ? 0 : NextArrival(snapshot_);
	Touch(table);
	NormalizeMemberStatus(table.p1); NormalizeMemberStatus(table.p2);
}

// First come, first paired: `member`, at no table, plays the player who has
// waited longest, else waits at an empty table. Two who just parted and find
// nobody else are paired again only when no table is left to wait at.
void RoomAuthority::PairArcade(MemberId member, MemberId parted) {
	Table* table = Waiter(member, parted, nullptr);
	for (auto& empty : snapshot_.tables) if (!table && Empty(empty)) table = &empty;
	if (!table) table = Waiter(member, 0, nullptr);
	if (!table) return;
	JoinTable(member, *table);
}

// A set is over. Arcade: the pair stays and each chooses Rematch or New
// opponent. King of the hill: the winner stays and plays whoever waited
// longest, the loser waits for the next one, and the streaks and the top
// slot follow.
void RoomAuthority::ArcadeSetEnded(Table& table, MemberId winner, MemberId loser) {
	if (snapshot_.arcade == ArcadeRoom) {
		table.deciding = true; table.rematch[0] = table.rematch[1] = false;
		return;
	}
	const int won = CabinetOf(snapshot_, winner), lost = CabinetOf(snapshot_, loser);
	if (won >= 0 && snapshot_.streaks[won] < 0xFFFF) ++snapshot_.streaks[won];
	if (lost >= 0) snapshot_.streaks[lost] = 0;
	CrownTopSlot();
	if (table.p1 == loser) table.p1 = 0; else table.p2 = 0;
	ClearReadiness(table);
	if (Table* challenger = Waiter(winner, loser, &table)) {
		const MemberId next = Lone(*challenger);
		challenger->p1 = challenger->p2 = 0; challenger->waitingSince = 0;
		Touch(*challenger);
		JoinTable(next, table);
	} else table.waitingSince = NextArrival(snapshot_);
	PairArcade(loser, winner);
	NormalizeMemberStatus(loser);
	TouchRoom();
}

// `left` stood up from `table`: the one still there plays whoever waited
// longest, or waits here.
void RoomAuthority::ArcadeVacated(Table& table, MemberId left) {
	table.deciding = false; table.rematch[0] = table.rematch[1] = false;
	const MemberId stays = Lone(table);
	if (!stays) { table.waitingSince = 0; return; }
	if (Table* waiter = Waiter(stays, left, &table)) {
		table.p1 = table.p2 = 0; table.waitingSince = 0;
		Touch(table);
		JoinTable(stays, *waiter);
	} else table.waitingSince = NextArrival(snapshot_);
}

// `member` leaves their cabinet, and its streak with it.
void RoomAuthority::StandUp(MemberId member) {
	const int cabinet = CabinetOf(snapshot_, member);
	if (cabinet < 0) return;
	snapshot_.cabinets[cabinet] = 0; snapshot_.streaks[cabinet] = 0;
	CrownTopSlot();
	TouchRoom();
}

// The top slot stays where it is until its streak ends or another is longer.
void RoomAuthority::CrownTopSlot() {
	auto& top = snapshot_.topSlot;
	if (snapshot_.arcade != KingRoom || (top >= 0 && !snapshot_.streaks[top])) top = -1;
	if (snapshot_.arcade != KingRoom) return;
	for (std::size_t i = 0; i < CabinetCount; ++i)
		if (snapshot_.streaks[i] > (top < 0 ? 0 : snapshot_.streaks[top])) top = static_cast<std::int8_t>(i);
}

// Queue names a cabinet: table + seat * TableCount. A member at no cabinet
// sits there and is paired; one at another cabinet moves, with their pair and
// streak, between games only.
Result RoomAuthority::ApplyArcadeSit(MemberId member, const Action& action) {
	if (action.seat != 0 && action.seat != 1) return Reject(RejectReason::InvalidSeat);
	const std::size_t cabinet = action.table + action.seat * TableCount;
	if (snapshot_.cabinets[cabinet]) return Reject(RejectReason::InvalidSeat);
	const int from = CabinetOf(snapshot_, member);
	if (from >= 0) {
		const auto* own = Find(member);
		if (own && own->table >= 0 && GameUnderWay(snapshot_.tables[own->table])) return Reject(RejectReason::WrongPhase);
		std::swap(snapshot_.cabinets[from], snapshot_.cabinets[cabinet]);
		std::swap(snapshot_.streaks[from], snapshot_.streaks[cabinet]);
		if (snapshot_.topSlot == from) snapshot_.topSlot = static_cast<std::int8_t>(cabinet);
		TouchRoom();
		return Accept();
	}
	for (const auto& table : snapshot_.tables)
		if (table.p1 == member || table.p2 == member) return Reject(RejectReason::AlreadySeated);
	StopWatching(member);
	snapshot_.cabinets[cabinet] = member; snapshot_.streaks[cabinet] = 0;
	PairArcade(member, 0);
	TouchRoom();
	NormalizeMemberStatus(member);
	return Accept();
}

// After a set: Rematch until both chose it, or New opponent, which parts the
// pair at once; each then plays whoever waited longest, or waits.
Result RoomAuthority::ApplyRematch(MemberId member, const Action& action, Table* table) {
	if (!snapshot_.arcade || !table->deciding) return Reject(RejectReason::WrongPhase);
	const int seat = table->p1 == member ? 0 : table->p2 == member ? 1 : -1;
	if (seat < 0) return Reject(RejectReason::NotSeated);
	if (action.kind == ActionKind::Rematch) {
		table->rematch[seat] = true;
		if (table->rematch[0] && table->rematch[1]) { table->deciding = false; table->rematch[0] = table->rematch[1] = false; }
		Touch(*table);
		return Accept();
	}
	const MemberId other = seat == 0 ? table->p2 : table->p1;
	table->p1 = table->p2 = 0;
	ClearReadiness(*table);
	table->deciding = false; table->rematch[0] = table->rematch[1] = false;
	table->phase = TablePhase::Idle;
	Touch(*table);
	PairArcade(member, other);
	PairArcade(other, member);
	NormalizeMemberStatus(member); NormalizeMemberStatus(other);
	return Accept();
}

// A new public room's host makes it an Arcade room while alone in it, with
// the rules of every table (netplay::CreatedRules).
Result RoomAuthority::ApplySetRoomType(MemberId member, const Action& action) {
	if (!snapshot_.serverOwned || !IsHost(member) || snapshot_.arcade || snapshot_.members.size() != 1 ||
		!action.roomType || action.roomType > KingRoom) return Reject(RejectReason::Unauthorized);
	if (!action.rules.Valid()) return Reject(RejectReason::InvalidRules);
	for (const auto& table : snapshot_.tables)
		if (!Empty(table) || !table.queue.empty()) return Reject(RejectReason::WrongPhase);
	snapshot_.arcade = action.roomType;
	for (auto& table : snapshot_.tables) { table.rules = action.rules; table.streakHolder = 0; table.streak = 0; Touch(table); }
	TouchRoom();
	return Accept();
}

} }
