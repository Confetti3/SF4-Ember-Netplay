// Arcade and King of the hill rooms: sixteen one-player cabinets; whoever sits
// down plays the player who has waited longest (Snapshot::type).
#include "room_authority_support.hxx"
#include "../common/RematchInput.hxx"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <vector>

// All eight tables unless `tables` says, as Create opens one.
static RoomAuthority Arcade(RoomType type = RoomType::Arcade, SetFormat format = SetFormat::Ft1, std::size_t tables = TableCount) {
	Rules rules;
	rules.format = format;
	return RoomAuthority("Arcade", 16, 1, rules, Charter{type, false, DelayPolicy::Shared, static_cast<std::uint8_t>(tables)});
}

// Queue names a cabinet: table + seat * TableCount.
static void Sit(RoomAuthority& authority, MemberId member, int cabinet, bool accepted = true) {
	Action sit = TableAction(authority, member, static_cast<std::uint8_t>(cabinet % TableCount), ActionKind::Queue);
	sit.seat = static_cast<std::int8_t>(cabinet / TableCount);
	CHECK(authority.Apply(member, sit).accepted == accepted);
}

static int TableOf(const RoomAuthority& authority, MemberId member) {
	for (const auto& t : authority.SnapshotView().tables) if (t.p1 == member || t.p2 == member) return t.id;
	return -1;
}
static bool Paired(const RoomAuthority& authority, MemberId a, MemberId b) {
	const int table = TableOf(authority, a);
	if (table < 0) return false;
	const auto& t = authority.SnapshotView().tables[table];
	return (t.p1 == a && t.p2 == b) || (t.p1 == b && t.p2 == a);
}
static bool Waits(const RoomAuthority& authority, MemberId member) { return Paired(authority, member, 0); }

static Result Choose(RoomAuthority& authority, MemberId member, ActionKind kind) {
	return authority.Apply(member, TableAction(authority, member, static_cast<std::uint8_t>(TableOf(authority, member)), kind));
}

// Both fighters at `member`'s table ready, play one game that `result`
// decides, and every recipient acknowledges it.
static void Play(RoomAuthority& authority, MemberId member, MatchResult result) {
	const auto table = static_cast<std::uint8_t>(TableOf(authority, member));
	const auto seated = authority.SnapshotView().tables[table];
	for (const auto fighter : {seated.p1, seated.p2}) CHECK(authority.Apply(fighter, TableAction(authority, fighter, table, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(table, seated.p1, seated.p2).accepted);
	const auto generation = authority.SnapshotView().tables[table].matchGeneration;
	CHECK(authority.EndMatch(table, generation, result).accepted);
	for (const auto recipient : authority.TerminalMembers(table, generation)) {
		Action acknowledgment = TableAction(authority, recipient, table, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		CHECK(authority.Apply(recipient, acknowledgment).accepted);
	}
}
// `winner` beats their opponent in a one-game set.
static void Beat(RoomAuthority& authority, MemberId winner) {
	const auto& t = authority.SnapshotView().tables[TableOf(authority, winner)];
	Play(authority, winner, t.p1 == winner ? MatchResult::P1Win : MatchResult::P2Win);
}

// Any free cabinet; the next to sit anywhere plays the one who waits, and a
// taken cabinet is refused. Sixteen members fill all eight tables.
static void TestFirstComeFirstPaired() {
	auto authority = Arcade();
	std::vector<MemberId> members;
	for (int i = 0; i < 16; ++i) members.push_back(Join(authority, i, i == 0));
	Sit(authority, members[0], 3);
	CHECK(Waits(authority, members[0]) && authority.SnapshotView().cabinets[3] == members[0]);
	Sit(authority, members[1], 3, false);
	Sit(authority, members[1], 12);
	CHECK(Paired(authority, members[0], members[1]) && StatusOf(authority, members[1]) == MemberStatus::Seated);
	int cabinet = 15;
	for (int i = 2; i < 16; ++i, --cabinet) { if (cabinet == 12) --cabinet; if (cabinet == 3) --cabinet; Sit(authority, members[i], cabinet); }
	for (int i = 0; i < 16; i += 2) CHECK(Paired(authority, members[i], members[i + 1]));
	for (const auto& t : authority.SnapshotView().tables) CHECK(t.phase == TablePhase::Waiting && t.queue.empty());
}

// A sit or stand-up on an old copy of its table is refused, as at any table,
// out of play or not; a fighter who readied can still stand up.
static void TestCabinetActionsKeepTableRules() {
	auto authority = Arcade(RoomType::Arcade, SetFormat::Ft1, 1);
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1, false), c = Join(authority, 2, false);
	Sit(authority, a, 0); Sit(authority, b, TableCount);
	Action stale = TableAction(authority, c, 3, ActionKind::Queue);
	--stale.tableRevision;
	CHECK(authority.Apply(c, stale).reason == RejectReason::StaleTable);
	Sit(authority, c, 3);
	CHECK(authority.SnapshotView().line == std::vector<MemberId>{c});
	CHECK(Choose(authority, a, ActionKind::Ready).accepted && Choose(authority, b, ActionKind::Ready).accepted);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Ready);
	CHECK(Choose(authority, a, ActionKind::Unqueue).accepted);
	CHECK(Paired(authority, b, c));
}

// Two pairs, so nobody waits alone: after a set the pair stays, Ready waits
// for both Rematches, then the pair plays again.
static void TestRematchBoth() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2), d = Join(authority, 3);
	Sit(authority, a, 0); Sit(authority, b, 9); Sit(authority, c, 2); Sit(authority, d, 10);
	Beat(authority, a);
	const auto table = TableOf(authority, a);
	CHECK(authority.SnapshotView().tables[table].deciding && Paired(authority, a, b));
	CHECK(Choose(authority, a, ActionKind::Ready).reason == RejectReason::WrongPhase);
	CHECK(Choose(authority, a, ActionKind::Rematch).accepted);
	CHECK(authority.SnapshotView().tables[table].deciding && authority.SnapshotView().tables[table].rematch[0]);
	CHECK(Choose(authority, b, ActionKind::Rematch).accepted);
	CHECK(!authority.SnapshotView().tables[table].deciding && Paired(authority, a, b));
	CHECK(Choose(authority, a, ActionKind::Ready).accepted);
	const auto wire = nlohmann::json(authority.SnapshotView());
	CHECK(!wire.at("tables")[table].contains("rematch"));
}

// One Rematch and one New opponent part the pair: each plays whoever waited
// longest, or waits for the next to sit, never with the other again while
// someone else waits. One who sits down while a pair chooses waits alone.
static void TestRematchOneRepairs() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2), d = Join(authority, 3), e = Join(authority, 4);
	const MemberId f = Join(authority, 5), g = Join(authority, 6);
	Sit(authority, a, 0); Sit(authority, b, 5); Sit(authority, f, 2); Sit(authority, g, 10);
	Beat(authority, a);
	CHECK(Choose(authority, a, ActionKind::Rematch).accepted);
	Sit(authority, c, 7);
	CHECK(Waits(authority, c) && authority.SnapshotView().tables[TableOf(authority, a)].deciding);
	CHECK(Choose(authority, b, ActionKind::NewOpponent).accepted);
	CHECK(Paired(authority, b, c) && Waits(authority, a));
	CHECK(Choose(authority, a, ActionKind::Rematch).reason == RejectReason::WrongPhase);
	// Two pairs and nobody alone: a finished pair chooses again. On New
	// opponent both wait, apart, and the next to sit plays the one who began
	// to wait first.
	Sit(authority, d, 15);
	CHECK(Paired(authority, d, a));
	Beat(authority, b);
	CHECK(Choose(authority, c, ActionKind::NewOpponent).accepted);
	CHECK(Waits(authority, c) && Waits(authority, b) && !Paired(authority, c, b) && Paired(authority, d, a));
	Sit(authority, e, 8);
	CHECK(Paired(authority, e, c) && Waits(authority, b));
	// Standing up leaves the cabinet; the opponent plays the one waiting.
	CHECK(Choose(authority, e, ActionKind::Unqueue).accepted);
	CHECK(authority.SnapshotView().cabinets[8] == 0 && TableOf(authority, e) < 0 && Paired(authority, c, b));
}

// A real arcade rotates: the winner stays at their table and plays whoever
// has waited alone longest; the loser waits, nobody chooses, and the loser
// plays the next one to sit down, not the winner.
static void TestArcadeWinnerPlaysWaiter() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2), d = Join(authority, 3);
	Sit(authority, a, 0); Sit(authority, b, 9); Sit(authority, c, 2);
	CHECK(Paired(authority, a, b) && Waits(authority, c));
	const int table = TableOf(authority, a);
	Beat(authority, a);
	const auto& room = authority.SnapshotView();
	CHECK(Paired(authority, a, c) && TableOf(authority, a) == table && Waits(authority, b));
	CHECK(room.tables[TableOf(authority, b)].waitingSince != 0);
	for (const auto& t : room.tables) CHECK(!t.deciding);
	Sit(authority, d, 4);
	CHECK(Paired(authority, d, b) && Paired(authority, a, c));
}

// Two players alone in the room: there is no new opponent to offer, so the
// pair is not asked and just readies again.
static void TestArcadeTwoPlayers() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1);
	Sit(authority, a, 0); Sit(authority, b, 9);
	Beat(authority, a);
	const auto& t = authority.SnapshotView().tables[TableOf(authority, a)];
	CHECK(Paired(authority, a, b) && !t.deciding && t.phase == TablePhase::Waiting);
	CHECK(Choose(authority, a, ActionKind::Rematch).reason == RejectReason::WrongPhase);
	CHECK(Choose(authority, a, ActionKind::Ready).accepted && Choose(authority, b, ActionKind::Ready).accepted);
}

// Two pairs and nobody alone: the pair that finishes stays and chooses, and
// the other pair plays on.
static void TestArcadePairsChoose() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2), d = Join(authority, 3);
	Sit(authority, a, 0); Sit(authority, b, 1); Sit(authority, c, 2); Sit(authority, d, 3);
	Beat(authority, a);
	const auto& room = authority.SnapshotView();
	CHECK(Paired(authority, a, b) && room.tables[TableOf(authority, a)].deciding);
	const auto& other = room.tables[TableOf(authority, c)];
	CHECK(Paired(authority, c, d) && !other.deciding && other.phase == TablePhase::Waiting);
}

// Five players, two pairs and one alone: each finished set rotates, since its
// loser is then the one waiting alone for the next set to end.
static void TestArcadeFiveRotate() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2), d = Join(authority, 3), e = Join(authority, 4);
	Sit(authority, a, 0); Sit(authority, b, 1); Sit(authority, c, 2); Sit(authority, d, 3); Sit(authority, e, 4);
	CHECK(Paired(authority, a, b) && Paired(authority, c, d) && Waits(authority, e));
	Beat(authority, a);
	CHECK(Paired(authority, a, e) && Paired(authority, c, d) && Waits(authority, b));
	Beat(authority, c);
	CHECK(Paired(authority, c, b) && Paired(authority, a, e) && Waits(authority, d));
	for (const auto& t : authority.SnapshotView().tables) CHECK(!t.deciding);
}

// King of the hill: the winner stays and plays the longest waiting, the loser
// waits; streaks count per cabinet, and the top slot goes to the longest one
// until someone beats that streak or beats its holder. A tie keeps it.
static void TestKingStreaksAndCrown() {
	auto authority = Arcade(RoomType::King);
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2), d = Join(authority, 3);
	Sit(authority, a, 0); Sit(authority, b, 1); Sit(authority, c, 2); Sit(authority, d, 3);
	const auto& room = authority.SnapshotView();
	Beat(authority, a);
	CHECK(room.streaks[0] == 1 && room.topSlot == 0 && Waits(authority, a) && Waits(authority, b));
	CHECK(!room.tables[TableOf(authority, a)].deciding);
	Beat(authority, c);
	CHECK(room.streaks[2] == 1 && room.topSlot == 0 && Paired(authority, c, a) && Paired(authority, d, b));
	Beat(authority, c);
	CHECK(room.streaks[2] == 2 && room.streaks[0] == 0 && room.topSlot == 2);
	Beat(authority, d);
	CHECK(room.streaks[3] == 1 && room.topSlot == 2 && Paired(authority, c, d));
	Beat(authority, d);
	CHECK(room.streaks[2] == 0 && room.streaks[3] == 2 && room.topSlot == 3);
	const auto wire = nlohmann::json(room);
	CHECK(wire.at("arcade") == 2 && wire.at("top_slot") == 3 && wire.get<Snapshot>().streaks[3] == 2);
	CHECK(authority.Leave(d).accepted && room.topSlot < 0 && room.cabinets[3] == 0);
}

// King of the hill with two players: the loser challenges again at once,
// the winner's streak going on.
static void TestKingTwoPlayers() {
	auto authority = Arcade(RoomType::King);
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1);
	Sit(authority, a, 0); Sit(authority, b, 8);
	const auto& room = authority.SnapshotView();
	Beat(authority, a);
	CHECK(room.streaks[0] == 1 && room.streaks[8] == 0 && Paired(authority, a, b));
	const auto& t = room.tables[TableOf(authority, a)];
	CHECK(t.phase == TablePhase::Waiting && t.score[0] == 0 && t.score[1] == 0 && !t.deciding);
	Beat(authority, a);
	CHECK(room.streaks[0] == 2 && room.topSlot == 0 && Paired(authority, a, b));
	Beat(authority, b);
	CHECK(room.streaks[0] == 0 && room.streaks[8] == 1 && room.topSlot == 8 && Paired(authority, a, b));
}

// An Arcade room's rules are the room's: no table's can be changed.
static void TestRulesAreTheRooms() {
	auto authority = Arcade();
	const MemberId host = Join(authority, 0, true);
	Action rules = TableAction(authority, host, 2, ActionKind::SetRules);
	rules.rules = authority.SnapshotView().tables[2].rules;
	rules.rules.roundCount = 5;
	CHECK(authority.Apply(host, rules).reason == RejectReason::Unauthorized);
}

// The room's type travels with its state; a tournament's Arcade room is
// refused, and so is a queue in one.
static void TestArcadeOnTheWire() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true);
	Sit(authority, a, 4);
	const auto wire = nlohmann::json(authority.SnapshotView());
	CHECK(wire.at("arcade") == 1 && wire.get<Snapshot>().cabinets[4] == a);
	CHECK(!nlohmann::json(RoomAuthority("Classic").SnapshotView()).contains("arcade"));
	RoomAuthority restored("Restore");
	CHECK(restored.RestoreCheckpoint(authority.Checkpoint()) && restored.SnapshotView().type == RoomType::Arcade && restored.SnapshotView().cabinets[4] == a);
	const auto refuses = [](nlohmann::json state) {
		try { (void)state.get<Snapshot>(); } catch (const std::exception&) { return true; }
		return false;
	};
	auto queued = wire;
	queued["tables"][3]["queue"] = nlohmann::json::array({a});
	auto stranger = wire;
	stranger["cabinets"][9] = 99;
	CHECK(refuses(queued) && refuses(stranger));
	TournamentBinding binding;
	binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	binding.assignmentGeneration = 1;
	binding.bindingRevision = 1;
	binding.gamesToWin = 2;
	binding.fighters[0] = {std::string(64, 'a'), "emb1_aaaa"};
	binding.fighters[1] = {std::string(64, 'b'), "emb1_bbbb"};
	CHECK(authority.BindTournament(binding).reason == RejectReason::Unauthorized);
}

// A public room may be either Arcade type: its creator, alone in it, sets the
// type and every table's rules in one action.
static void TestPublicArcade() {
	RoomAuthority authority("Public", 16, 1);
	CHECK(authority.SetServerOwned());
	MemberProfile profile;
	profile.admission.account = "ember-a";
	const auto joined = authority.Join("Creator", Peer(1), false, profile);
	CHECK(joined.accepted);
	const MemberId host = joined.snapshot.members.back().id;
	Action type = TableAction(authority, host, 0, ActionKind::SetRoomType);
	type.roomType = RoomType::King;
	type.rules.format = SetFormat::Ft2;
	type.tableCount = 1;
	CHECK(nlohmann::json(type).get<Action>().roomType == RoomType::King);
	CHECK(authority.Apply(host, type).accepted);
	const auto& room = authority.SnapshotView();
	CHECK(room.type == RoomType::King && room.tables[7].rules.format == SetFormat::Ft2 && TablesInPlay(room) == 1);
	CHECK(nlohmann::json(room).get<Snapshot>().type == RoomType::King);
	type = TableAction(authority, host, 0, ActionKind::SetRoomType);
	type.roomType = RoomType::Arcade;
	CHECK(!authority.Apply(host, type).accepted);
	Sit(authority, host, 6);
	// Empty, it is a Classic room at the default count again.
	CHECK(authority.Leave(host).accepted && room.type == RoomType::Classic && room.tableCount == ClassicTableCount && room.line.empty());
	auto arcade = Arcade();
	CHECK(arcade.SetServerOwned());
}

// An Arcade room may also be a Practice room.
static void TestArcadePracticeRoom() {
	RoomAuthority authority("Both", 8, 1, Rules(), Charter{RoomType::Arcade, true});
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1);
	Sit(authority, a, 0); Sit(authority, b, 2);
	CHECK(authority.SnapshotView().practice && Paired(authority, a, b));
}

// The room's option keeps a set's winner on their fighter (KeepsFighter)
// while they stay at the table, never the loser, and never without the
// option; each member's sets won and lost count up and cross the wire.
static void TestWinnerKeepsFighterAndRecords() {
	for (const bool option : {true, false}) {
		Rules rules;
		rules.format = SetFormat::Ft1;
		rules.winnerKeepsFighter = option;
		RoomAuthority authority("Arcade", 16, 1, rules, Charter{RoomType::Arcade});
		const MemberId a = Join(authority, 0, true), b = Join(authority, 1);
		Sit(authority, a, 0); Sit(authority, b, 9);
		CHECK(authority.SetMemberChara(a, {5, 14}) && authority.SetMemberChara(b, {7, 14}));
		CHECK(!KeepsFighter(authority.SnapshotView(), a) && !KeepsFighter(authority.SnapshotView(), b));
		Beat(authority, a);
		const auto& room = authority.SnapshotView();
		CHECK(KeepsFighter(room, a) == option && !KeepsFighter(room, b));
		// The host takes only the kept fighter and edition from the keeper;
		// anything from the loser, and any Ultra or look (not checked here).
		CHECK(authority.FighterAllowed(a, 5, 14) && authority.FighterAllowed(a, 6, 14) == !option &&
			authority.FighterAllowed(a, 5, 13) == !option && authority.FighterAllowed(b, 9, 13));
		const auto* winner = FindMember(room, a);
		const auto* loser = FindMember(room, b);
		CHECK(winner && loser && winner->setsWon == 1 && winner->setsLost == 0 && loser->setsWon == 0 && loser->setsLost == 1);
		const auto wire = nlohmann::json(room);
		const auto restored = wire.get<Snapshot>();
		CHECK(FindMember(restored, a)->setsWon == 1 && FindMember(restored, b)->setsLost == 1);
		CHECK(restored.tables[TableOf(authority, a)].rules.winnerKeepsFighter == option);
		// Off, the rule is not written, so the wire stays as it was.
		CHECK(wire.at("tables")[TableOf(authority, a)].at("rules").contains("winner_keeps_fighter") == option);
		CHECK(restored.tables[TableOf(authority, a)].keeper == (option ? a : 0) && KeepsFighter(restored, a) == option);
		// Standing up ends it: sitting down again, even at the same table, keeps nothing.
		CHECK(Choose(authority, a, ActionKind::Unqueue).accepted);
		CHECK(!KeepsFighter(authority.SnapshotView(), a) && authority.FighterAllowed(a, 6, 13));
		Sit(authority, a, 0);
		CHECK(!KeepsFighter(authority.SnapshotView(), a));
	}
	// Never in a Classic room, whatever its rules say.
	Rules rules;
	rules.winnerKeepsFighter = true;
	Snapshot classic;
	classic.tables[0].rules = rules;
	CHECK(!KeepsFighter(classic, 1));
}

// The client offers a rematch in place only where the authority leaves the set
// open: an Arcade room with no set length plays sets of one game, so its pair
// must rotate rather than restart (a third player would never get a turn).
static void TestRematchInPlaceFollowsSetGoal() {
	Snapshot snapshot;
	auto& table = snapshot.tables[0];
	for (const auto type : {RoomType::Arcade, RoomType::King}) {
		snapshot.type = type;
		table.rules.format = SetFormat::Unlimited;
		table.score[0] = table.score[1] = 0;
		CHECK(SetGoal(snapshot, table) == 1);
		CHECK(!RematchInPlaceOffered(snapshot, table, MatchResult::P1Win));
		CHECK(!RematchInPlaceOffered(snapshot, table, MatchResult::P2Win));
		CHECK(RematchInPlaceOffered(snapshot, table, MatchResult::Draw));
	}
	snapshot.type = RoomType::Arcade;
	table.rules.format = SetFormat::Ft2;
	CHECK(SetGoal(snapshot, table) == 2);
	CHECK(RematchInPlaceOffered(snapshot, table, MatchResult::P1Win));   // 1-0 mid-set
	table.score[0] = 1;
	CHECK(!RematchInPlaceOffered(snapshot, table, MatchResult::P1Win));  // wins the set
	CHECK(RematchInPlaceOffered(snapshot, table, MatchResult::P2Win));
	// Classic rooms keep an unlimited set open.
	snapshot.type = RoomType::Classic;
	table.rules.format = SetFormat::Unlimited;
	CHECK(SetGoal(snapshot, table) == 0);
	CHECK(RematchInPlaceOffered(snapshot, table, MatchResult::P1Win));
}

// The post-match prompt's set score counts the game just played before the
// room has it: a game held for a rematch in place reaches the room only once
// the next one is fought, and a set's last game only once both reports agree.
static void TestStandingCountsTheGameJustPlayed() {
	Snapshot snapshot;
	snapshot.type = RoomType::Arcade;
	auto& table = snapshot.tables[0];
	table.rules.format = SetFormat::Ft3;
	table.matchGeneration = 7; table.phase = TablePhase::Playing;
	table.score[0] = 1; table.score[1] = 0;
	// 2-0 in a first to 3, the room still at 1-0: the set goes on.
	auto standing = StandingAfter(snapshot, table, 7, MatchResult::P1Win);
	CHECK(standing.score[0] == 2 && standing.score[1] == 0 && !standing.over);
	// The game that wins the set, before the room ends it.
	table.score[0] = 2;
	standing = StandingAfter(snapshot, table, 7, MatchResult::P1Win);
	CHECK(standing.score[0] == 3 && standing.over);
	// A result not known yet, and a draw, count for nothing.
	CHECK(StandingAfter(snapshot, table, 7, MatchResult::Abort).score[0] == 2 && !StandingAfter(snapshot, table, 7, MatchResult::Abort).over);
	CHECK(StandingAfter(snapshot, table, 7, MatchResult::Draw).score[0] == 2);
	// The room has it: the set it ended, whose score starts again at 0-0.
	table.score[0] = 0; table.phase = TablePhase::Waiting;
	table.lastSet.generation = 7; table.lastSet.score[0] = 3; table.lastSet.score[1] = 0;
	standing = StandingAfter(snapshot, table, 7, MatchResult::P1Win);
	CHECK(standing.score[0] == 3 && standing.score[1] == 0 && standing.over);
	// A game inside the set the room has counted is not counted twice.
	table.lastSet.generation = 6; table.score[0] = 1;
	standing = StandingAfter(snapshot, table, 7, MatchResult::P1Win);
	CHECK(standing.score[0] == 1 && !standing.over);
}

// Over a WIN the battle may go on from, each fighter's prompt rides in their
// input. While either is still asked the WIN waits for them, so an answer in
// the prompt's last second, or a frame predicted without it, cannot end the
// battle before both chose Rematch.
static void TestWinWaitsForTheRematchAnswers() {
	using namespace sf4e::rematch;
	CHECK(LocalBits(false, true, 0) == 0);
	CHECK(LocalBits(true, false, 0) == AskingBit);
	CHECK(LocalBits(true, false, AskingMs - 1) == AskingBit);
	CHECK(LocalBits(true, false, AskingMs) == 0);   // the prompt's time is up: no answer
	CHECK(LocalBits(true, true, AskingMs * 3) == ChoiceBit);
	CHECK(Decide(ChoiceBit, ChoiceBit) == Win::End);
	CHECK(Decide(ChoiceBit, AskingBit) == Win::Hold);  // one chose, the other is still asked
	CHECK(Decide(AskingBit, 0) == Win::Hold);
	CHECK(Decide(ChoiceBit, 0) == Win::Run);           // the other gave no answer in time
	CHECK(Decide(0, 0) == Win::Run);
	// The bits sit outside the fight buttons the game reads.
	CHECK((Bits & 0x0FFFFFFFu) == 0);
}

// A game that goes on in place keeps the pick the battle loaded, so a Rematch
// with another one (an Ultra changed on the prompt) leaves the battle and the
// next game loads it.
static void TestChangedPickLeavesTheBattle() {
	using namespace sf4e::rematch;
	sf4e::selection::Pick played;
	played.fighter = 7; played.ultra = 0; played.costume = 2; played.color = 4;
	CHECK(GoesOnInPlace(played, played));
	for (const int ultra : {1, 2}) {
		auto next = played; next.ultra = ultra;
		CHECK(!GoesOnInPlace(played, next));
	}
	auto next = played; next.color = 5;
	CHECK(!GoesOnInPlace(played, next));
	next = played; next.fighter = 8;
	CHECK(!GoesOnInPlace(played, next));
}

// A full room: sixteen at eight tables, nobody alone and no table free. New
// opponent is always on offer. With nobody else choosing it waits: the next
// set to end elsewhere sends its loser here and the one who asked plays the
// winner. With another fighter choosing, it swaps at once.
static void TestFullRoomNewOpponent() {
	auto authority = Arcade();
	std::vector<MemberId> m;
	for (int i = 0; i < 16; ++i) m.push_back(Join(authority, i, i == 0));
	for (int i = 0; i < 16; ++i) Sit(authority, m[i], i);
	const MemberId a = m[0], b = Paired(authority, a, m[8]) ? m[8] : m[1];
	const MemberId c = m[2], d = Paired(authority, c, m[10]) ? m[10] : m[3];
	const MemberId e = m[4], f = Paired(authority, e, m[12]) ? m[12] : m[5];
	CHECK(Paired(authority, a, b) && Paired(authority, c, d) && Paired(authority, e, f));
	Beat(authority, a);
	CHECK(authority.SnapshotView().tables[TableOf(authority, a)].deciding);
	// Nobody else is choosing: the request waits, the pair stays.
	CHECK(Choose(authority, a, ActionKind::NewOpponent).accepted);
	const auto& asked = authority.SnapshotView().tables[TableOf(authority, a)];
	CHECK(Paired(authority, a, b) && asked.wantsNew[asked.p1 == a ? 0 : 1]);
	const auto wire = nlohmann::json(authority.SnapshotView());
	CHECK(wire.at("tables")[TableOf(authority, a)].contains("wants_new"));
	// Another set ends: its loser goes to a's partner, a plays the winner.
	const MemberId winner = c, loser = d;
	Beat(authority, winner);
	CHECK(Paired(authority, winner, a) && Paired(authority, loser, b));
	for (const auto& t : authority.SnapshotView().tables) CHECK(!t.deciding && !t.wantsNew[0] && !t.wantsNew[1]);
	// Two pairs choosing at once: New opponent never pulls in a fighter who
	// has not asked, so it waits; once one at the other table asks too, the
	// two who asked play each other's last opponents.
	Beat(authority, e);
	Beat(authority, a);
	const MemberId aOpponent = Paired(authority, a, winner) ? winner : loser;
	CHECK(Choose(authority, a, ActionKind::NewOpponent).accepted && Paired(authority, a, aOpponent) && Paired(authority, e, f));
	CHECK(Choose(authority, e, ActionKind::NewOpponent).accepted);
	CHECK(Paired(authority, a, f) && Paired(authority, e, aOpponent));
	// Answering Rematch instead withdraws the request.
	Beat(authority, winner);
	const auto& mine = authority.SnapshotView().tables[TableOf(authority, winner)];
	const int seat = mine.p1 == winner ? 0 : 1;
	CHECK(Choose(authority, winner, ActionKind::NewOpponent).accepted && mine.wantsNew[seat]);
	CHECK(Choose(authority, winner, ActionKind::Rematch).accepted && !mine.wantsNew[seat] && mine.rematch[seat]);
}

static MemberId OpponentOf(const RoomAuthority& authority, MemberId member) {
	const int table = TableOf(authority, member);
	if (table < 0) return 0;
	const auto& t = authority.SnapshotView().tables[table];
	return t.p1 == member ? t.p2 : t.p1;
}
static int Asking(const RoomAuthority& authority) {
	int asking = 0;
	for (const auto& t : authority.SnapshotView().tables) asking += t.wantsNew[0] + t.wantsNew[1];
	return asking;
}
static std::vector<MemberId> FullRoom(RoomAuthority& authority) {
	std::vector<MemberId> m;
	for (int i = 0; i < 16; ++i) m.push_back(Join(authority, i, i == 0));
	for (int i = 0; i < 16; ++i) Sit(authority, m[i], i);
	for (const auto& t : authority.SnapshotView().tables) CHECK(t.p1 && t.p2);
	return m;
}

// A request that waits in a full room is answered as soon as anyone waits
// alone, not only when a set ends: a fighter stands up, and the one who asked
// plays their partner at once. The one left behind waits and plays the next
// to sit down.
static void TestFullRoomRequestTakesAFreedSeat() {
	auto authority = Arcade();
	const auto m = FullRoom(authority);
	const MemberId a = m[0], b = OpponentOf(authority, a);
	Beat(authority, a);
	CHECK(Choose(authority, a, ActionKind::NewOpponent).accepted && Asking(authority) == 1 && Paired(authority, a, b));
	MemberId x = 0;
	for (const auto member : m) if (member != a && member != b) { x = member; break; }
	const MemberId y = OpponentOf(authority, x);
	const int cabinet = CabinetOf(authority.SnapshotView(), x);
	CHECK(Choose(authority, x, ActionKind::Unqueue).accepted);
	CHECK(Paired(authority, a, y) && Waits(authority, b) && Asking(authority) == 0);
	for (const auto& t : authority.SnapshotView().tables) CHECK(!t.deciding);
	Sit(authority, x, cabinet);
	CHECK(Paired(authority, x, b));
}

// A waiting request is served by the next set to end, or by a second request
// at another table, never by pulling in a fighter who did not ask. One who
// asked and stands up takes the request with them; the partner then waits.
static void TestFullRoomTwoRequestsSwap() {
	auto authority = Arcade();
	const auto m = FullRoom(authority);
	const MemberId a = m[0], b = OpponentOf(authority, a);
	MemberId c = 0;
	for (const auto member : m) if (member != a && member != b) { c = member; break; }
	const MemberId d = OpponentOf(authority, c);
	Beat(authority, a);
	CHECK(Choose(authority, a, ActionKind::NewOpponent).accepted && Asking(authority) == 1);
	// c's set ends while a waits: a's request is served by it at once.
	Beat(authority, c);
	CHECK(Paired(authority, a, c) && Paired(authority, b, d) && Asking(authority) == 0);
	// Both pairs finish. A Rematch vote at the other table is never taken
	// back by a request here: it waits. When the other fighter there asks
	// too, the two who asked trade places.
	Beat(authority, a);
	Beat(authority, b);
	CHECK(Choose(authority, b, ActionKind::Rematch).accepted);
	CHECK(Choose(authority, a, ActionKind::NewOpponent).accepted && Asking(authority) == 1 && Paired(authority, a, c));
	const auto& other = authority.SnapshotView().tables[TableOf(authority, b)];
	CHECK(Paired(authority, b, d) && other.deciding && other.rematch[other.p1 == b ? 0 : 1]);
	CHECK(Choose(authority, d, ActionKind::NewOpponent).accepted && Asking(authority) == 0);
	CHECK(Paired(authority, a, b) && Paired(authority, d, c));
	for (const auto& t : authority.SnapshotView().tables) CHECK(!t.deciding);
	// A request leaves with the one who made it; the partner waits alone.
	Beat(authority, a);
	const MemberId partner = OpponentOf(authority, a);
	CHECK(Choose(authority, a, ActionKind::NewOpponent).accepted && Asking(authority) == 1);
	CHECK(Choose(authority, a, ActionKind::Unqueue).accepted);
	CHECK(Asking(authority) == 0 && Waits(authority, partner));
}

// What holds after every step, whoever does what: a cabinet holder is at one
// table, a pair carries no wait stamp and a lone fighter does, choices and
// requests live on a deciding pair only, one table at most asks and only
// while nobody is free, at most the two who just parted wait alone (a third
// would have been paired with one of them), the top slot is the longest
// streak, a free cabinet has no streak, and member rows mirror the tables.
static void Sound(const RoomAuthority& authority, const std::string& what) {
	const auto& r = authority.SnapshotView();
	const auto sound = [&](bool holds, const char* rule) { CHECK_MSG(holds, "%s, after %s", rule, what.c_str()); };
	std::map<MemberId, int> seats;
	int asking = 0;
	for (const auto& t : r.tables) {
		for (const auto m : {t.p1, t.p2}) if (m) ++seats[m];
		const bool pair = t.p1 && t.p2;
		sound(pair ? t.waitingSince == 0 : (!t.p1 && !t.p2) || t.waitingSince > 0, "wait stamp");
		sound(t.deciding ? pair : !t.wantsNew[0] && !t.wantsNew[1] && !t.rematch[0] && !t.rematch[1], "choices on a deciding pair");
		sound(t.phase == (pair ? TablePhase::Waiting : TablePhase::Idle), "table phase");
		asking += t.wantsNew[0] || t.wantsNew[1];
	}
	sound(asking <= 1, "one table asks");
	const auto inPlay = r.tables.begin() + static_cast<std::ptrdiff_t>(TablesInPlay(r));
	sound(!asking || (r.line.empty() && std::none_of(r.tables.begin(), inPlay, [](const Table& t) { return ArcadeAlone(t) || ArcadeEmpty(t); })),
		"a request waits while nobody is free");
	sound(std::count_if(r.tables.begin(), r.tables.end(), ArcadeAlone) <= 2, "two alone at most");
	for (const auto& seat : seats) sound(seat.second == 1 && CabinetOf(r, seat.first) >= 0, "a seat has a cabinet");
	for (std::size_t i = 0; i < CabinetCount; ++i) {
		const bool seated = r.cabinets[i] && seats[r.cabinets[i]] == 1;
		sound(r.cabinets[i] ? seated != (ArcadeLinePlace(r, r.cabinets[i]) != 0) : r.streaks[i] == 0, "a cabinet has a table or a place in line");
	}
	for (const auto m : r.line) sound(CabinetOf(r, m) >= 0 && ArcadeLinePlace(r, m) == static_cast<int>(std::find(r.line.begin(), r.line.end(), m) - r.line.begin()) + 1, "the line holds cabinets once each");
	const bool line = !r.line.empty();
	for (std::size_t i = 0; i < TableCount; ++i) {
		const auto& t = r.tables[i];
		if (i >= TablesInPlay(r)) sound(ArcadeEmpty(t), "a table out of play is empty");
		else sound(!line || (!ArcadeAlone(t) && !ArcadeEmpty(t)), "the line waits only while every table in play is a pair");
	}
	for (const auto s : r.streaks) sound(r.topSlot < 0 ? s == 0 : s <= r.streaks[r.topSlot] && r.streaks[r.topSlot] > 0, "top slot");
	for (const auto& m : r.members) {
		const int table = TableOf(authority, m.id);
		sound(m.table == table && (table < 0 ? m.seat < 0 && m.status == MemberStatus::Idle :
			m.seat == (r.tables[table].p1 == m.id ? 0 : 1) && m.status == MemberStatus::Seated), "member row");
	}
}

// One table, as in the game's Endless Battle: the winner stays, whoever has waited longest
// plays them, and the loser goes to the back of the line. Leaving the line,
// a seat coming free and a table added each seat the line in order, and the
// line goes on the wire.
static void TestOneTableLine() {
	for (const RoomType type : {RoomType::Arcade, RoomType::King}) {
		auto authority = Arcade(type, SetFormat::Ft1, 1);
		const auto& r = authority.SnapshotView();
		const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2), d = Join(authority, 3), e = Join(authority, 4);
		Sit(authority, a, 0); Sit(authority, b, 8); Sit(authority, c, 5); Sit(authority, d, 6);
		const auto place = [&](MemberId m) { return ArcadeLinePlace(r, m); };
		CHECK(Paired(authority, a, b) && place(c) == 1 && place(d) == 2 && TableOf(authority, c) < 0);
		CHECK(ArcadeEmpty(r.tables[5]) && PlaceOf(r, c).kind == Place::Kind::Line && PlaceOf(r, c).position == 1);
		Beat(authority, a);
		CHECK(Paired(authority, a, c) && place(d) == 1 && place(b) == 2);
		Beat(authority, c);
		CHECK(Paired(authority, c, d) && place(b) == 1 && place(a) == 2);
		if (type == RoomType::King) CHECK(r.streaks[CabinetOf(r, c)] == 1 && r.streaks[CabinetOf(r, a)] == 0);
		const auto wire = nlohmann::json(r);
		CHECK(wire.contains("line") && wire.get<Snapshot>().line == r.line);
		Sit(authority, e, 7);
		CHECK(place(e) == 3);
		CHECK(authority.Leave(b).accepted && place(a) == 1 && place(e) == 2);
		CHECK(Choose(authority, d, ActionKind::Unqueue).accepted && Paired(authority, c, a) && place(e) == 1);
		// Leaving the line by its cabinet, and sitting back down at the back.
		CHECK(authority.Apply(e, TableAction(authority, e, 7, ActionKind::Unqueue)).accepted && CabinetOf(r, e) < 0);
		Sit(authority, e, 7);
		CHECK(place(e) == 1);
		CHECK(authority.Apply(a, [&] { Action grow = TableAction(authority, a, 0, ActionKind::SetTableCount); grow.tableCount = 2; return grow; }()).accepted);
		CHECK(Waits(authority, e) && TableOf(authority, e) == 1);
		Sound(authority, "the one-table line");
	}
}

// Gagapa's room: sixteen fighters, sets ending one table after another, and
// every winner asking for a new opponent. Nobody is kept on the same opponent:
// at most one request waits at a time, each fighter meets several others, and
// nobody who asked is paired with the one they asked to leave.
static void TestFullRoomRotates() {
	auto authority = Arcade();
	const auto m = FullRoom(authority);
	std::map<MemberId, std::set<MemberId>> met;
	std::map<MemberId, MemberId> leftBehind;
	for (int step = 0; step < 96; ++step) {
		const auto& tables = authority.SnapshotView().tables;
		const auto& t = tables[step % tables.size()];
		if (t.deciding || !t.p1 || !t.p2) continue;
		const MemberId winner = step % 3 ? t.p1 : t.p2;
		const MemberId loser = OpponentOf(authority, winner);
		Beat(authority, winner);
		Sound(authority, "a set end in the rotation");
		met[winner].insert(loser); met[loser].insert(winner);
		if (authority.SnapshotView().tables[TableOf(authority, winner)].deciding) {
			CHECK(Choose(authority, winner, ActionKind::NewOpponent).accepted);
			if (Paired(authority, winner, loser)) leftBehind[winner] = loser;
		}
		CHECK(Asking(authority) <= 1);
		for (const auto& left : leftBehind)
			if (!authority.SnapshotView().tables[TableOf(authority, left.first)].deciding) CHECK(OpponentOf(authority, left.first) != left.second);
		for (auto it = leftBehind.begin(); it != leftBehind.end();)
			it = authority.SnapshotView().tables[TableOf(authority, it->first)].deciding ? std::next(it) : leftBehind.erase(it);
	}
	for (const auto member : m) CHECK(met[member].size() >= 3);
}

// King of the hill in a full room: the loser has no table to wait at, so the
// pair stays and the loser asks for a new opponent at once; the next set to
// end elsewhere trades the losers, and both streaks go on. Rematch from both
// keeps the pair instead.
static void TestFullRoomKing() {
	auto authority = Arcade(RoomType::King);
	const auto m = FullRoom(authority);
	const auto& room = authority.SnapshotView();
	const MemberId a = m[0], b = OpponentOf(authority, a);
	Beat(authority, a);
	const auto& t = room.tables[TableOf(authority, a)];
	CHECK(Paired(authority, a, b) && t.deciding && t.wantsNew[t.p1 == b ? 0 : 1] && Asking(authority) == 1);
	CHECK(room.streaks[0] == 1 && room.topSlot == 0);
	MemberId c = 0;
	for (const auto member : m) if (member != a && member != b) { c = member; break; }
	const MemberId d = OpponentOf(authority, c);
	Beat(authority, c);
	CHECK(Paired(authority, a, d) && Paired(authority, c, b) && Asking(authority) == 0);
	CHECK(room.streaks[0] == 1 && room.streaks[CabinetOf(room, c)] == 1);
	for (const auto& x : room.tables) CHECK(!x.deciding);
	Beat(authority, a);
	CHECK(Choose(authority, d, ActionKind::Rematch).accepted && Choose(authority, a, ActionKind::Rematch).accepted);
	CHECK(Paired(authority, a, d) && !t.deciding && Asking(authority) == 0 && room.streaks[0] == 2);
	Sound(authority, "the King pair's Rematch");
}

// A random walk through a room: members join, sit anywhere, stand up and
// leave now and then, sets end either way, deciding pairs answer. After each
// step the room is sound; a winner keeps the table; the loser leaves it
// whenever a table is free (King) or someone else sits (Arcade, asked
// first); New opponent with someone free parts the pair.
static void TestRandomWalk(RoomType type, std::size_t tables = TableCount) {
	std::uint32_t rng = 2026u + type + 97u * static_cast<std::uint32_t>(tables);
	const auto roll = [&](int n) { rng = rng * 1664525u + 1013904223u; return static_cast<int>((rng >> 8) % n); };
	for (int round = 0; round < 60; ++round) {
		auto authority = Arcade(type, SetFormat::Ft1, tables);
		std::vector<MemberId> members{Join(authority, 0, true)};
		int joined = 1;
		for (int step = 0; step < 300; ++step) {
			const auto& r = authority.SnapshotView();
			std::vector<MemberId> deciding, playing, unseated;
			for (const auto& t : r.tables) for (const auto m : {t.p1, t.p2}) if (m && t.p2 && t.p1) (t.deciding ? deciding : playing).push_back(m);
			for (const auto m : members) if (CabinetOf(r, m) < 0) unseated.push_back(m);
			const auto pick = [&](const std::vector<MemberId>& from) { return from[roll(static_cast<int>(from.size()))]; };
			const int op = roll(100);
			std::string what = "nothing";
			if (op < 4 && members.size() > 1) {
				const MemberId m = pick(members);
				if (m != members[0]) { CHECK(authority.Leave(m).accepted); members.erase(std::find(members.begin(), members.end(), m)); what = "leave"; }
			} else if (op < 8 && !playing.empty()) { CHECK(Choose(authority, pick(playing), ActionKind::Unqueue).accepted); what = "stand up"; }
			else if (op < 20 && members.size() < 16) { members.push_back(Join(authority, joined++)); what = "join"; }
			else if (op < 40 && !unseated.empty()) {
				std::vector<int> open;
				for (int i = 0; i < static_cast<int>(CabinetCount); ++i) if (!r.cabinets[i]) open.push_back(i);
				Sit(authority, pick(unseated), open[roll(static_cast<int>(open.size()))]);
				what = "sit";
			} else if (op < 70 && !deciding.empty()) {
				const MemberId m = pick(deciding), other = OpponentOf(authority, m);
				const bool free = ArcadeSomeoneFree(r, static_cast<std::size_t>(TableOf(authority, m)));
				const auto kind = roll(2) ? ActionKind::Rematch : ActionKind::NewOpponent;
				CHECK(Choose(authority, m, kind).accepted);
				if (kind == ActionKind::NewOpponent && free) CHECK(!Paired(authority, m, other));
				what = kind == ActionKind::Rematch ? "rematch" : "new opponent";
			} else if (!playing.empty()) {
				const MemberId m = pick(playing), other = OpponentOf(authority, m);
				const int table = TableOf(authority, m);
				const bool free = ArcadeSomeoneFree(r, static_cast<std::size_t>(table));
				const bool others = std::any_of(r.cabinets.begin(), r.cabinets.end(), [&](MemberId c) { return c && c != m && c != other; });
				Beat(authority, m);
				CHECK(TableOf(authority, m) == table);
				const auto& after = r.tables[table];
				if (Paired(authority, m, other)) CHECK(!others || after.deciding);
				if (type == RoomType::King && others && free) CHECK(!Paired(authority, m, other));
				if (type == RoomType::King && others && Paired(authority, m, other)) CHECK(after.wantsNew[after.p1 == other ? 0 : 1]);
				what = "set end";
			}
			Sound(authority, what + " at step " + std::to_string(step) + " of round " + std::to_string(round));
		}
	}
}

// Each room type's rules, as the room and every view read them, and the
// numbers the wire and saved settings hold for each type.
static void TestTypeRules() {
	CHECK(static_cast<int>(RoomType::Classic) == 0 && static_cast<int>(RoomType::Arcade) == 1 && static_cast<int>(RoomType::King) == 2);
	CHECK(ValidRoomType(0) && ValidRoomType(2) && !ValidRoomType(3) && !ValidRoomType(-1));
	const auto& classic = RulesOf(RoomType::Classic);
	CHECK(!classic.cabinets && !classic.rulesFixed && !classic.setAlwaysEnds && !classic.winnerStays && !classic.keeperOption &&
		classic.tournamentBindable && classic.defaultTableCount == ClassicTableCount);
	for (const RoomType type : {RoomType::Arcade, RoomType::King}) {
		const auto& rules = RulesOf(type);
		CHECK(rules.cabinets && rules.rulesFixed && rules.setAlwaysEnds && rules.keeperOption && !rules.tournamentBindable &&
			rules.defaultTableCount == TableCount && rules.winnerStays == (type == RoomType::King));
		CHECK(DefaultTableCount(type) == TableCount);
	}
	CHECK(DefaultTableCount(RoomType::Classic) == ClassicTableCount);
	// A value no snapshot holds reads as any type but Classic did before.
	CHECK(&RulesOf(static_cast<RoomType>(7)) == &RulesOf(RoomType::Arcade));
	Snapshot room;
	room.type = static_cast<RoomType>(3);
	CHECK(!ArcadeKindValid(room));
}

// A cabinet's Queue names it as table + seat * TableCount, and the room reads
// it back the same way; only in a room that seats by cabinet do Queue and
// Unqueue name one.
static void TestCabinetNaming() {
	for (std::size_t cabinet = 0; cabinet < CabinetCount; ++cabinet) {
		const auto action = CabinetQueueAction(cabinet);
		CHECK(action.kind == ActionKind::Queue && action.table < TableCount && (action.seat == 0 || action.seat == 1) && CabinetNamed(action) == cabinet);
	}
	Snapshot room;
	CHECK(!NamesCabinet(room, ActionKind::Queue) && !NamesCabinet(room, ActionKind::Unqueue));
	room.type = RoomType::King;
	CHECK(NamesCabinet(room, ActionKind::Queue) && NamesCabinet(room, ActionKind::Unqueue) && !NamesCabinet(room, ActionKind::Watch));
}

// Who keeps the seat with no answer after a game: everyone in a room that
// seats by cabinet; nobody at an Unlimited Classic table; both inside a set;
// after a set, whoever the rotation keeps.
static void TestKeepsSeatAfterGame() {
	Snapshot room;
	auto& table = room.tables[0];
	table.p1 = 1; table.p2 = 2; table.phase = TablePhase::Playing; table.matchGeneration = 5;
	table.rules.format = SetFormat::Unlimited;
	CHECK(!KeepsSeatAfterGame(room, table, 5, MatchResult::P1Win, 1));
	table.rules.format = SetFormat::Ft2;
	CHECK(KeepsSeatAfterGame(room, table, 5, MatchResult::P1Win, 1) && KeepsSeatAfterGame(room, table, 5, MatchResult::P1Win, 2));
	table.score[0] = 1;
	table.rules.rotation = RotationMode::WinnerStays;
	CHECK(KeepsSeatAfterGame(room, table, 5, MatchResult::P1Win, 1) && !KeepsSeatAfterGame(room, table, 5, MatchResult::P1Win, 2));
	table.rules.rotation = RotationMode::LoserStays;
	CHECK(!KeepsSeatAfterGame(room, table, 5, MatchResult::P1Win, 1) && KeepsSeatAfterGame(room, table, 5, MatchResult::P1Win, 2));
	CHECK(GameSeats(table, 5) == std::make_pair(MemberId(1), MemberId(2)));
	room.type = RoomType::Arcade;
	table.rules.format = SetFormat::Unlimited;
	CHECK(KeepsSeatAfterGame(room, table, 5, MatchResult::P1Win, 2));
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	try {
		TestTypeRules();
		TestCabinetNaming();
		TestKeepsSeatAfterGame();
		TestFirstComeFirstPaired();
		TestCabinetActionsKeepTableRules();
		TestRematchBoth();
		TestRematchOneRepairs();
		TestFullRoomNewOpponent();
		TestFullRoomRequestTakesAFreedSeat();
		TestFullRoomTwoRequestsSwap();
		TestFullRoomRotates();
		TestFullRoomKing();
		TestRandomWalk(RoomType::Arcade);
		TestRandomWalk(RoomType::King);
		TestOneTableLine();
		for (const std::size_t tables : {1, 3}) { TestRandomWalk(RoomType::Arcade, tables); TestRandomWalk(RoomType::King, tables); }
		TestArcadeWinnerPlaysWaiter();
		TestArcadeTwoPlayers();
		TestArcadePairsChoose();
		TestArcadeFiveRotate();
		TestKingStreaksAndCrown();
		TestKingTwoPlayers();
		TestRulesAreTheRooms();
		TestArcadeOnTheWire();
		TestWinnerKeepsFighterAndRecords();
		TestPublicArcade();
		TestArcadePracticeRoom();
		TestRematchInPlaceFollowsSetGoal();
		TestStandingCountsTheGameJustPlayed();
		TestWinWaitsForTheRematchAnswers();
		TestChangedPickLeavesTheBattle();
	} catch (const std::exception& error) {
		std::printf("room arcade threw: %s\n", error.what());
		return 1;
	}
	if (TEST_FAILURES()) std::printf("%d room arcade check(s) failed\n", TEST_FAILURES());
	else std::printf("room arcade: ok\n");
	return TEST_FAILURES() ? 1 : 0;
}
