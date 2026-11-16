// An Arcade or King of the hill room's seating (Snapshot::type): sixteen
// one-player cabinets, and tables that are pairs, not places. Whoever sits
// down plays the player who has waited longest, at whichever table in play is
// free; with none free they wait in line (Snapshot::line). At one table the
// winner stays, as in the game's Endless Battle, and the loser goes to the
// back of the line.
// Other rooms never reach this code.
#include "RoomModel.hxx"
#include "RoomQueries.hxx"

#include <algorithm>

namespace sf4e { namespace room {

namespace {
// The pair's choice after a set is over, or never began.
void EndChoice(Table& table) {
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

// A fighter who asked for a new opponent does not sit with the one they left
// again until they have sat with someone else, nor the other with them, while
// anyone else at the cabinets can still come free to play either: they wait,
// in Training if they like. With nobody else there they play each other, as
// a pair alone in the room does.
bool RoomAuthority::MayMeet(MemberId a, MemberId b) const {
	const auto left = [&](MemberId from, MemberId other) {
		const auto at = partedFrom_.find(from);
		return at != partedFrom_.end() && at->second == other;
	};
	return !(left(a, b) || left(b, a)) || !OthersAtCabinets(snapshot_, a, b);
}

// The table of the player who has waited longest for `member`, not counting
// the one they just parted from or one they may not meet.
Table* RoomAuthority::Waiter(MemberId member, MemberId parted, const Table* except) {
	Table* longest = nullptr;
	for (auto& table : snapshot_.tables) {
		if (&table == except || !ArcadeAlone(table) || Lone(table) == member) continue;
		if (Lone(table) == parted || !MayMeet(member, Lone(table))) continue;
		if (!longest || table.waitingSince < longest->waitingSince) longest = &table;
	}
	return longest;
}

// The first in line who may play `member`, or 0. Whoever may not keeps their place.
MemberId RoomAuthority::NextInLine(MemberId member) const {
	for (const MemberId next : snapshot_.line) if (next != member && MayMeet(member, next)) return next;
	return 0;
}

// `member` takes the open seat at `table`: a new pair at 0-0, or one waiting.
void RoomAuthority::JoinTable(MemberId member, Table& table) {
	// Moved to another table, a keeper keeps nothing (Table::keeper).
	for (auto& other : snapshot_.tables)
		if (&other != &table && other.keeper == member) { other.keeper = 0; other.keptFighter = other.keptEdition = -1; }
	auto& line = snapshot_.line;
	line.erase(std::remove(line.begin(), line.end(), member), line.end());
	if (!table.p1) table.p1 = member; else table.p2 = member;
	ClearReadiness(table);
	table.score[0] = table.score[1] = 0;
	EndChoice(table);
	const bool pair = table.p1 && table.p2;
	// A new pair: whoever either of them left before, they have now sat with someone (MayMeet).
	if (pair) { partedFrom_.erase(table.p1); partedFrom_.erase(table.p2); }
	table.phase = pair ? TablePhase::Waiting : TablePhase::Idle;
	table.waitingSince = pair ? 0 : NextArrival(snapshot_);
	Touch(table);
	NormalizeMemberStatus(table.p1); NormalizeMemberStatus(table.p2);
}

// First come, first paired: `member`, at no table, joins the back of the
// line, and the line is seated (SeatLine). So they play the player who has
// waited longest, or wait at an empty table, or in line.
void RoomAuthority::PairArcade(MemberId member, MemberId parted) {
	if (CabinetOf(snapshot_, member) < 0) return;
	auto& line = snapshot_.line;
	if (std::find(line.begin(), line.end(), member) == line.end()) line.push_back(member);
	SeatLine(member, parted);
	// Still in line: a pair waiting for a new opponent parts, and the one who
	// asked plays whoever has waited longest.
	if (std::find(line.begin(), line.end(), member) == line.end()) return;
	const auto asker = ArcadeAsker(snapshot_, TableCount);  // at any table
	if (asker.member) PartPair(snapshot_.tables[asker.table], asker.member);
}

// The line, front first, sits down while the tables in play have room:
// beside whoever waits alone there and may meet them, else at an empty table
// to wait. `a` and `b`, a pair that just parted, are not seated together again.
void RoomAuthority::SeatLine(MemberId a, MemberId b) {
	for (const MemberId member : std::vector<MemberId>(snapshot_.line)) {
		Table* table = Waiter(member, member == a ? b : member == b ? a : 0, nullptr);
		for (std::size_t i = 0; !table && i < TablesInPlay(snapshot_); ++i)
			if (ArcadeEmpty(snapshot_.tables[i])) table = &snapshot_.tables[i];
		if (table) JoinTable(member, *table);
	}
}

// `leaving` at `table` and `asker`, waiting at another pair for a new
// opponent (ArcadeAsker), trade places: each pair gets someone new, at 0-0.
void RoomAuthority::SwapFighter(MemberId leaving, Table& table, const ArcadeWaitingAsker& asker) {
	Table& other = snapshot_.tables[asker.table];
	(other.p1 == asker.member ? other.p1 : other.p2) = 0;
	(table.p1 == leaving ? table.p1 : table.p2) = 0;
	JoinTable(asker.member, table);
	JoinTable(leaving, other);
	TouchRoom();
}

// A set is over. The winner stays and plays whoever has waited longest, alone
// or in line, of those they may meet (MayMeet), here; the loser waits for the
// next one (PairArcade), not the winner. With nobody waiting the pair stays:
// an Arcade pair chooses Rematch or New opponent, and with nobody else there
// just readies again. King of the hill
// moves the streaks and the top slot, and while a table is free the loser
// waits there and the winner here for a challenger; in a full room the pair
// stays like an Arcade one, the loser already asking for a new opponent.
void RoomAuthority::ArcadeSetEnded(Table& table, MemberId winner, MemberId loser) {
	const bool king = RulesOf(snapshot_).winnerStays;
	if (king) {
		const int won = CabinetOf(snapshot_, winner), lost = CabinetOf(snapshot_, loser);
		if (won >= 0 && snapshot_.streaks[won] < 0xFFFF) ++snapshot_.streaks[won];
		if (lost >= 0) snapshot_.streaks[lost] = 0;
		CrownTopSlot();
		TouchRoom();
	}
	Table* const challenger = Waiter(winner, loser, &table);
	const MemberId waiting = challenger ? 0 : NextInLine(winner);
	const bool others = OthersAtCabinets(snapshot_, winner, loser);
	const bool parts = challenger || waiting || (king && others && ArcadeSomeoneFree(snapshot_, table.id));
	if (!parts) {
		// Someone at another table asked for a new opponent and nobody was free:
		// the loser goes there, and the one who asked plays the winner.
		const auto asker = ArcadeAsker(snapshot_, table.id);
		if (asker.member) { SwapFighter(loser, table, asker); return; }
		EndChoice(table);
		table.deciding = others;
		if (king && others) table.wantsNew[table.p1 == loser ? 0 : 1] = true;
		return;
	}
	if (table.p1 == loser) table.p1 = 0; else table.p2 = 0;
	ClearReadiness(table);
	if (challenger) {
		const MemberId next = Lone(*challenger);
		challenger->p1 = challenger->p2 = 0; challenger->waitingSince = 0;
		Touch(*challenger);
		JoinTable(next, table);
	} else if (waiting) JoinTable(waiting, table);
	else table.waitingSince = NextArrival(snapshot_);
	PairArcade(loser, winner);
	NormalizeMemberStatus(loser);
	TouchRoom();
}

// `left` stood up from `table` and from their cabinet (RemoveFromTable): the
// one still there plays whoever waited longest, or the fighter waiting for a
// new opponent, or waits here. A seat coming free is the one way someone is
// free while a request waits: requests wait only in a full room, and a set
// that ends answers them itself.
void RoomAuthority::ArcadeVacated(Table& table, MemberId left) {
	EndChoice(table);
	const MemberId stays = Lone(table);
	if (!stays) { table.waitingSince = 0; SeatLine(0, 0); return; }
	if (Table* waiter = Waiter(stays, left, &table)) {
		table.p1 = table.p2 = 0; table.waitingSince = 0;
		Touch(table);
		JoinTable(stays, *waiter);
		// Only someone who may not meet that waiter can be in line: this table is theirs now.
		SeatLine(0, 0);
		return;
	}
	if (const MemberId next = NextInLine(stays)) { JoinTable(next, table); return; }
	table.waitingSince = NextArrival(snapshot_);
	const auto asker = ArcadeAsker(snapshot_, table.id);
	if (asker.member) PartPair(snapshot_.tables[asker.table], asker.member);
}

// The pair at `table` parts: `member`, who asked to, plays whoever has waited
// longest, and so does the other, or each waits alone. Neither is seated with
// the other again while the one who asked has sat with nobody else, nor while
// the other has if they asked too (MayMeet).
void RoomAuthority::PartPair(Table& table, MemberId member) {
	const MemberId other = table.p1 == member ? table.p2 : table.p1;
	partedFrom_[member] = other;
	if (ArcadeAsked(table, table.p1 == other ? 0 : 1)) partedFrom_[other] = member;
	table.p1 = table.p2 = 0;
	ClearReadiness(table);
	EndChoice(table);
	table.phase = TablePhase::Idle;
	Touch(table);
	PairArcade(member, other);
	PairArcade(other, member);
	NormalizeMemberStatus(member); NormalizeMemberStatus(other);
	TouchRoom();
}

// `member` leaves their cabinet, and its streak with it, and nobody waits to
// avoid them any more.
void RoomAuthority::StandUp(MemberId member) {
	const int cabinet = CabinetOf(snapshot_, member);
	if (cabinet < 0) return;
	snapshot_.cabinets[cabinet] = 0; snapshot_.streaks[cabinet] = 0;
	snapshot_.line.erase(std::remove(snapshot_.line.begin(), snapshot_.line.end(), member), snapshot_.line.end());
	partedFrom_.erase(member);
	for (auto at = partedFrom_.begin(); at != partedFrom_.end();)
		if (at->second == member) at = partedFrom_.erase(at); else ++at;
	CrownTopSlot();
	TouchRoom();
}

// The top slot stays where it is until its streak ends or another is longer.
void RoomAuthority::CrownTopSlot() {
	auto& top = snapshot_.topSlot;
	const bool streaks = RulesOf(snapshot_).winnerStays;
	if (!streaks || (top >= 0 && !snapshot_.streaks[top])) top = -1;
	if (!streaks) return;
	for (std::size_t i = 0; i < CabinetCount; ++i)
		if (snapshot_.streaks[i] > (top < 0 ? 0 : snapshot_.streaks[top])) top = static_cast<std::int8_t>(i);
}

// Queue names a cabinet: table + seat * TableCount. A member at no cabinet
// sits there and is paired; one at another cabinet moves, with their pair and
// streak, between games only.
Result RoomAuthority::ApplyArcadeSit(MemberId member, const Action& action) {
	if (HasOutstandingTerminalReceiptForMember(member)) return Reject(RejectReason::TerminalLedgerFull);
	if (action.seat != 0 && action.seat != 1) return Reject(RejectReason::InvalidSeat);
	const std::size_t cabinet = CabinetNamed(action);
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
	return Accept();
}

// `member` stands up from their cabinet, from a table between games or from
// the line.
Result RoomAuthority::ApplyArcadeStand(MemberId member) {
	const Member* own = Find(member);
	if (!own || CabinetOf(snapshot_, member) < 0) return Reject(RejectReason::NotQueued);
	if (own->table >= 0 && (snapshot_.tables[own->table].phase == TablePhase::Playing || snapshot_.tables[own->table].phase == TablePhase::Paused))
		return Reject(RejectReason::WrongPhase);
	RemoveFromTable(member);
	return Accept();
}

// After a set: Rematch until both chose it, or New opponent. With someone
// else free, New opponent parts the pair at once and each plays whoever waited
// longest, or waits. In a full room it swaps with the fighter waiting for a
// new opponent at another table, or else waits (Table::wantsNew) for the next
// set to end or seat to come free.
Result RoomAuthority::ApplyRematch(MemberId member, const Action& action, Table* table) {
	if (!RulesOf(snapshot_).cabinets || !table->deciding) return Reject(RejectReason::WrongPhase);
	const int seat = table->p1 == member ? 0 : table->p2 == member ? 1 : -1;
	if (seat < 0) return Reject(RejectReason::NotSeated);
	if (action.kind == ActionKind::Rematch) {
		table->rematch[seat] = true; table->wantsNew[seat] = false;
		if (table->rematch[0] && table->rematch[1]) EndChoice(*table);
		Touch(*table);
		return Accept();
	}
	switch (ArcadeNewOpponentFor(snapshot_, table->id)) {
	case ArcadeNewOpponent::PartNow: PartPair(*table, member); break;
	case ArcadeNewOpponent::Swap: SwapFighter(member, *table, ArcadeAsker(snapshot_, table->id)); break;
	case ArcadeNewOpponent::Wait: table->wantsNew[seat] = true; table->rematch[seat] = false; Touch(*table); break;
	}
	return Accept();
}

// A new public room's host makes it an Arcade room while alone in it, with
// the rules of every table and how many tables it plays at
// (netplay::CreatedRules).
Result RoomAuthority::ApplySetRoomType(MemberId member, const Action& action) {
	if (!snapshot_.serverOwned || !IsHost(member) || snapshot_.type != RoomType::Classic || snapshot_.members.size() != 1 ||
		action.roomType == RoomType::Classic || !ValidRoomType(static_cast<int>(action.roomType))) return Reject(RejectReason::Unauthorized);
	if (!action.rules.Valid() || action.tableCount < 1 || action.tableCount > TableCount) return Reject(RejectReason::InvalidRules);
	for (const auto& table : snapshot_.tables)
		if (!ArcadeEmpty(table) || !table.queue.empty()) return Reject(RejectReason::WrongPhase);
	snapshot_.type = action.roomType;
	snapshot_.tableCount = action.tableCount;
	for (auto& table : snapshot_.tables) { table.rules = action.rules; table.streakHolder = 0; table.streak = 0; Touch(table); }
	TouchRoom();
	return Accept();
}

} }
