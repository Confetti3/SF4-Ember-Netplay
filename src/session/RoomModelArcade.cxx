// An Arcade or King of the hill room's seating (Snapshot::arcade): sixteen
// one-player cabinets, and tables that are pairs, not places. Whoever sits
// down plays the player who has waited longest, at whichever table is free.
// Other rooms never reach this code.
#include "RoomModel.hxx"
#include "RoomQueries.hxx"

#include <algorithm>

namespace sf4e { namespace room {

namespace {
void ClearVotes(Table& table) {
	table.deciding = false;
	table.rematch[0] = table.rematch[1] = false;
	table.wantsNew[0] = table.wantsNew[1] = false;
}
MemberId Lone(const Table& table) { return table.p1 ? table.p1 : table.p2; }
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
		if (&table == except || !ArcadeAlone(table) || Lone(table) == member) continue;
		if (parted && Lone(table) == parted) continue;
		if (!longest || table.waitingSince < longest->waitingSince) longest = &table;
	}
	return longest;
}

// `member` takes the open seat at `table`: a new pair at 0-0, or one waiting.
void RoomAuthority::JoinTable(MemberId member, Table& table) {
	// Moved to another table, a keeper keeps nothing (Table::keeper).
	for (auto& other : snapshot_.tables)
		if (&other != &table && other.keeper == member) { other.keeper = 0; other.keptFighter = other.keptEdition = -1; }
	if (!table.p1) table.p1 = member; else table.p2 = member;
	ClearReadiness(table);
	table.score[0] = table.score[1] = 0;
	ClearVotes(table);
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
	for (auto& empty : snapshot_.tables) if (!table && ArcadeEmpty(empty)) table = &empty;
	if (!table) table = Waiter(member, 0, nullptr);
	if (!table) return;
	JoinTable(member, *table);
}

// `leaving` at `table` and the fighter at `far` (table * 2 + seat) trade
// places: each pair gets someone new, at 0-0.
void RoomAuthority::SwapFighter(MemberId leaving, Table& table, int far) {
	Table& other = snapshot_.tables[far / 2];
	MemberId& theirs = far % 2 ? other.p2 : other.p1;
	const MemberId stranger = theirs;
	(table.p1 == leaving ? table.p1 : table.p2) = 0;
	theirs = 0;
	JoinTable(stranger, table);
	JoinTable(leaving, other);
}

// A set is over. The winner stays and plays whoever has waited longest alone,
// here; the loser waits for the next one (PairArcade), not the winner. With
// nobody waiting alone the pair stays: an Arcade pair chooses Rematch or New
// opponent, and with nobody else there just readies again. King of the hill
// moves the streaks and the top slot, and while anyone else sits at a
// cabinet the winner waits here for them instead.
void RoomAuthority::ArcadeSetEnded(Table& table, MemberId winner, MemberId loser) {
	const bool king = snapshot_.arcade == KingRoom;
	if (king) {
		const int won = CabinetOf(snapshot_, winner), lost = CabinetOf(snapshot_, loser);
		if (won >= 0 && snapshot_.streaks[won] < 0xFFFF) ++snapshot_.streaks[won];
		if (lost >= 0) snapshot_.streaks[lost] = 0;
		CrownTopSlot();
		TouchRoom();
	}
	Table* const challenger = Waiter(winner, loser, &table);
	const bool others = std::any_of(snapshot_.cabinets.begin(), snapshot_.cabinets.end(),
		[&](MemberId m) { return m && m != winner && m != loser; });
	const bool parts = challenger || (king && others);
	if (!parts) {
		ClearVotes(table);
		// Someone at another table asked for a new opponent and nobody was free:
		// the loser goes there, and the one who asked plays the winner.
		const int asked = king ? -1 : ArcadeSwapSeat(snapshot_, table.id, true);
		if (asked >= 0) {
			SwapFighter(loser, table, asked);
			TouchRoom();
			return;
		}
		table.deciding = !king && others;
		return;
	}
	if (table.p1 == loser) table.p1 = 0; else table.p2 = 0;
	ClearReadiness(table);
	if (challenger) {
		const MemberId next = Lone(*challenger);
		challenger->p1 = challenger->p2 = 0; challenger->waitingSince = 0;
		Touch(*challenger);
		JoinTable(next, table);
	} else table.waitingSince = NextArrival(snapshot_);
	PairArcade(loser, winner);
	NormalizeMemberStatus(loser);
	TouchRoom();
	ServeNewOpponents();
}

// `left` stood up from `table`: the one still there plays whoever waited
// longest, or waits here.
void RoomAuthority::ArcadeVacated(Table& table, MemberId left) {
	ClearVotes(table);
	const MemberId stays = Lone(table);
	if (!stays) { table.waitingSince = 0; return; }
	if (Table* waiter = Waiter(stays, left, &table)) {
		table.p1 = table.p2 = 0; table.waitingSince = 0;
		Touch(table);
		JoinTable(stays, *waiter);
	} else table.waitingSince = NextArrival(snapshot_);
	ServeNewOpponents();
}

// The pair at `table` parts: `member` plays whoever has waited longest, and so
// does the other, or each waits alone.
void RoomAuthority::PartPair(Table& table, MemberId member) {
	const MemberId other = table.p1 == member ? table.p2 : table.p1;
	table.p1 = table.p2 = 0;
	ClearReadiness(table);
	ClearVotes(table);
	table.phase = TablePhase::Idle;
	Touch(table);
	PairArcade(member, other);
	PairArcade(other, member);
	NormalizeMemberStatus(member); NormalizeMemberStatus(other);
}

// Someone now waits alone (a seat came free, a set parted its pair, a member
// sat down) while a fighter at a full pair still waits for the New opponent
// they asked for (Table::wantsNew). That pair parts as New opponent does with
// someone free, so the request never waits for a set to end when a fighter is
// already there. Each pass answers one request, and the partner left alone may
// answer the next.
void RoomAuthority::ServeNewOpponents() {
	if (snapshot_.arcade != ArcadeRoom) return;
	for (;;) {
		if (std::none_of(snapshot_.tables.begin(), snapshot_.tables.end(), [](const Table& t) { return ArcadeAlone(t); })) return;
		Table* asked = nullptr; MemberId member = 0;
		for (auto& t : snapshot_.tables) {
			if (asked || !t.deciding || !t.p1 || !t.p2) continue;
			if (t.wantsNew[0]) { asked = &t; member = t.p1; }
			else if (t.wantsNew[1]) { asked = &t; member = t.p2; }
		}
		if (!asked) return;
		PartPair(*asked, member);
		TouchRoom();
	}
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
	// Moving between cabinets above is not sitting down: a member already at
	// one keeps playing under a Wired players only rule that came on since.
	if (!MaySit(member)) return Reject(RejectReason::WiredOnly);
	StopWatching(member);
	snapshot_.cabinets[cabinet] = member; snapshot_.streaks[cabinet] = 0;
	PairArcade(member, 0);
	TouchRoom();
	NormalizeMemberStatus(member);
	ServeNewOpponents();
	return Accept();
}

// After a set: Rematch until both chose it, or New opponent. With someone
// else free, New opponent parts the pair at once and each plays whoever waited
// longest, or waits. In a full room it swaps with a fighter choosing at another
// table, or else waits (Table::wantsNew) for the next set that ends.
Result RoomAuthority::ApplyRematch(MemberId member, const Action& action, Table* table) {
	if (!snapshot_.arcade || !table->deciding) return Reject(RejectReason::WrongPhase);
	const int seat = table->p1 == member ? 0 : table->p2 == member ? 1 : -1;
	if (seat < 0) return Reject(RejectReason::NotSeated);
	if (action.kind == ActionKind::Rematch) {
		table->rematch[seat] = true; table->wantsNew[seat] = false;
		if (table->rematch[0] && table->rematch[1]) ClearVotes(*table);
		Touch(*table);
		return Accept();
	}
	if (!ArcadeFreeTable(snapshot_, table->id)) {
		const int swap = ArcadeSwapSeat(snapshot_, table->id);
		if (swap >= 0) { SwapFighter(member, *table, swap); TouchRoom(); }
		else { table->wantsNew[seat] = true; table->rematch[seat] = false; Touch(*table); }
		return Accept();
	}
	PartPair(*table, member);
	ServeNewOpponents();
	return Accept();
}

// A new public room's host makes it an Arcade room while alone in it, with
// the rules of every table (netplay::CreatedRules).
Result RoomAuthority::ApplySetRoomType(MemberId member, const Action& action) {
	if (!snapshot_.serverOwned || !IsHost(member) || snapshot_.arcade || snapshot_.members.size() != 1 ||
		!action.roomType || action.roomType > KingRoom) return Reject(RejectReason::Unauthorized);
	if (!action.rules.Valid()) return Reject(RejectReason::InvalidRules);
	for (const auto& table : snapshot_.tables)
		if (!ArcadeEmpty(table) || !table.queue.empty()) return Reject(RejectReason::WrongPhase);
	snapshot_.arcade = action.roomType;
	for (auto& table : snapshot_.tables) { table.rules = action.rules; table.streakHolder = 0; table.streak = 0; Touch(table); }
	TouchRoom();
	return Accept();
}

} }
