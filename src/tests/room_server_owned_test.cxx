#include "room_authority_support.hxx"

// A server-owned room: no player hosts it, the first joiner moderates, and an
// Ember ID (the account) is the identity a kick and a duplicate join key on.

static MemberProfile Profile(const std::string& account) {
	MemberProfile profile;
	profile.admission.account = account;
	return profile;
}

static Result JoinAs(RoomAuthority& authority, int index, const std::string& account) {
	return authority.Join("Player" + std::to_string(index), Peer(index), false, Profile(account));
}

static MemberId MemberOf(const Result& result) {
	return result.accepted ? result.snapshot.members.back().id : 0;
}

static RoomAuthority NewRoom(std::uint64_t epoch = 1) {
	RoomAuthority authority("Public", 8, epoch);
	EXPECT(authority.SetServerOwned());
	return authority;
}

static void TestHostIsFirstJoinerThenOldest() {
	auto authority = NewRoom();
	EXPECT(authority.SnapshotView().serverOwned && authority.SnapshotView().host == 0);
	const auto first = JoinAs(authority, 1, "ember-a");
	EXPECT(first.accepted);
	const auto a = MemberOf(first);
	EXPECT(authority.SnapshotView().host == a && FindMember(authority.SnapshotView(), a)->host);
	const auto b = MemberOf(JoinAs(authority, 2, "ember-b"));
	const auto c = MemberOf(JoinAs(authority, 3, "ember-c"));
	EXPECT(b && c && authority.SnapshotView().host == a && !FindMember(authority.SnapshotView(), b)->host);
	// The host argument means nothing here: a second "host" is an ordinary member.
	const auto claimed = authority.Join("Claimer", Peer(4), true, Profile("ember-d"));
	EXPECT(claimed.accepted && authority.SnapshotView().host == a && !claimed.snapshot.members.back().host);
	EXPECT(authority.Leave(a).accepted);
	EXPECT(authority.SnapshotView().host == b && FindMember(authority.SnapshotView(), b)->host);
	EXPECT(!FindMember(authority.SnapshotView(), c)->host);
}

static void TestEmptyRoomStaysOpen() {
	auto authority = NewRoom();
	const auto a = MemberOf(JoinAs(authority, 1, "ember-a"));
	const auto b = MemberOf(JoinAs(authority, 2, "ember-b"));
	EXPECT(authority.Apply(a, [&] {
		auto lock = TableAction(authority, a, 0, ActionKind::Lock);
		lock.locked = true;
		return lock;
	}()).accepted);
	EXPECT(authority.SnapshotView().locked);
	// Seat both, so leaving also has to free the table.
	EXPECT(authority.Apply(a, TableAction(authority, a, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.Apply(b, TableAction(authority, b, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.SnapshotView().tables[0].phase == TablePhase::Waiting);
	EXPECT(authority.Leave(a).accepted);
	const auto last = authority.Leave(b);
	EXPECT(last.accepted);
	for (const auto& event : last.events) EXPECT(event.kind != Event::Kind::RoomClosed);
	const auto& snapshot = authority.SnapshotView();
	EXPECT(!snapshot.closed && !snapshot.locked && snapshot.host == 0 && snapshot.members.empty() && snapshot.serverOwned);
	EXPECT(snapshot.tables[0].phase == TablePhase::Idle && snapshot.tables[0].p1 == 0 && snapshot.tables[0].p2 == 0);
	EXPECT(authority.KickedAccounts().empty());
	const auto next = JoinAs(authority, 3, "ember-c");
	EXPECT(next.accepted && authority.SnapshotView().host == MemberOf(next));
	EXPECT(FindMember(authority.SnapshotView(), MemberOf(next))->host);
}

static void TestKickKeysOnTheAccount() {
	auto authority = NewRoom();
	const auto host = MemberOf(JoinAs(authority, 1, "ember-host"));
	const auto target = MemberOf(JoinAs(authority, 2, "ember-bad"));
	auto kick = TableAction(authority, host, 0, ActionKind::Kick);
	kick.target = target;
	EXPECT(authority.Apply(host, kick).accepted);
	EXPECT(authority.KickedAccounts() == std::vector<std::string>{"ember-bad"});
	// A new connection with the same account is still refused.
	const auto again = authority.Join("Back", Peer(20), false, Profile("ember-bad"));
	EXPECT(!again.accepted && again.reason == RejectReason::MemberKicked);
	// Someone else on the kicked member's old connection is not.
	const auto other = authority.Join("Other", Peer(2), false, Profile("ember-good"));
	EXPECT(other.accepted);
	EXPECT(authority.SnapshotView().members.size() == 2);
}

// Bans are never evicted: they are kept up to the lifetime cap, and the kick
// that would exceed it removes its target and closes the room instead.
static void TestBansAreKeptThenTheKickAtTheCapClosesTheRoom() {
	auto authority = NewRoom();
	const auto host = MemberOf(JoinAs(authority, 1, "ember-host"));
	const auto account = [](std::size_t i) { return "ember-banned-" + std::to_string(i); };
	const auto kickNext = [&](std::size_t i) {
		const auto target = MemberOf(JoinAs(authority, 100 + static_cast<int>(i), account(i)));
		EXPECT(target != 0);
		auto kick = TableAction(authority, host, 0, ActionKind::Kick);
		kick.target = target;
		return std::make_pair(target, authority.Apply(host, kick));
	};
	for (std::size_t i = 0; i < MaximumKickedAccounts; ++i) {
		const auto kicked = kickNext(i);
		EXPECT(kicked.second.accepted && !authority.SnapshotView().closed);
	}
	EXPECT(authority.KickedAccounts().size() == MaximumKickedAccounts);
	// Nothing was evicted: the first and the last ban still refuse a rejoin.
	for (const auto index : {std::size_t(0), MaximumKickedAccounts - 1}) {
		const auto again = authority.Join("Back", Peer(9000), false, Profile(account(index)));
		EXPECT(!again.accepted && again.reason == RejectReason::MemberKicked);
	}
	// The next kick removes the member, records no ban and closes the room.
	const auto last = kickNext(MaximumKickedAccounts);
	EXPECT(last.second.accepted && authority.SnapshotView().closed);
	EXPECT(FindMember(authority.SnapshotView(), last.first) == nullptr);
	EXPECT(authority.KickedAccounts().size() == MaximumKickedAccounts);
	const auto has = [&](Event::Kind kind) {
		return std::any_of(last.second.events.begin(), last.second.events.end(), [kind](const Event& event) { return event.kind == kind; });
	};
	EXPECT(has(Event::Kind::MemberRemoved) && has(Event::Kind::RoomClosed));
	EXPECT(last.second.events.back().kind == Event::Kind::RoomClosed && last.second.snapshot.closed);
	for (const auto& table : authority.SnapshotView().tables) EXPECT(table.phase == TablePhase::Closed);
	EXPECT(!authority.Join("Late", Peer(9001), false, Profile("ember-late")).accepted);
	// The closed room's checkpoint restores; one ban over the cap does not.
	const auto checkpoint = authority.Checkpoint();
	RoomAuthority restored("Other");
	EXPECT(restored.RestoreCheckpoint(checkpoint) && restored.KickedAccounts() == authority.KickedAccounts());
	auto over = checkpoint;
	over["kicked_accounts"].push_back("ember-one-too-many");
	EXPECT(!RoomAuthority("Other").RestoreCheckpoint(over));
}

// A room with a host, two fighters mid-match at table 0 and a bystander.
struct MatchRoom {
	RoomAuthority authority = NewRoom();
	MemberId host = 0, p1 = 0, p2 = 0, bystander = 0;
};

static void StartMatchRoom(MatchRoom& room) {
	auto& authority = room.authority;
	room.host = MemberOf(JoinAs(authority, 1, "ember-host"));
	room.p1 = MemberOf(JoinAs(authority, 2, "ember-p1"));
	room.p2 = MemberOf(JoinAs(authority, 3, "ember-p2"));
	room.bystander = MemberOf(JoinAs(authority, 4, "ember-bystander"));
	EXPECT(room.host && room.p1 && room.p2 && room.bystander);
	for (const auto member : {room.p1, room.p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	for (const auto member : {room.p1, room.p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, room.p1, room.p2).accepted);
	EXPECT(authority.SnapshotView().tables[0].phase == TablePhase::Playing);
}

static Result KickBy(RoomAuthority& authority, MemberId host, MemberId target) {
	auto kick = TableAction(authority, host, 0, ActionKind::Kick);
	kick.target = target;
	return authority.Apply(host, kick);
}

// The ban follows the removal. A kick of a fighter mid-match stores an abort
// receipt first; when the terminal ledger has no room Leave refuses, and then
// the member stays and nobody is banned.
static void TestKickBansOnlyAfterTheMemberIsRemoved() {
	// Ordinary order: a fighter mid-match is removed and then banned.
	{
		MatchRoom room;
		StartMatchRoom(room);
		auto& authority = room.authority;
		EXPECT(KickBy(authority, room.host, room.p1).accepted);
		EXPECT(FindMember(authority.SnapshotView(), room.p1) == nullptr && FindMember(authority.SnapshotView(), room.p2) != nullptr);
		EXPECT(authority.KickedAccounts() == std::vector<std::string>{"ember-p1"});
		EXPECT(!authority.Join("Back", Peer(20), false, Profile("ember-p1")).accepted);
	}

	// A full ledger: restore the room with the maximum number of unacknowledged
	// receipts on the idle tables, which is the only way a kick's Leave refuses.
	MatchRoom room;
	StartMatchRoom(room);
	auto& authority = room.authority;
	auto checkpoint = authority.Checkpoint();
	auto recipients = checkpoint.at("active_match_recipients").at(0);
	EXPECT(recipients.is_array() && recipients.size() == 2);
	for (auto& row : recipients) row["acknowledged"] = false;
	auto receipts = nlohmann::json::array();
	std::size_t generation = 1;
	std::uint8_t table = 1;
	while (receipts.size() < 64) {
		receipts.push_back({{"table", table}, {"generation", generation}, {"result", 0},
			{"fighters", {room.p1, room.p2}}, {"recipients", recipients}, {"acknowledged", false}});
		if (++table == 4) { table = 1; ++generation; }
	}
	for (std::size_t i = 1; i < 4; ++i) checkpoint["snapshot"]["tables"][i]["match_generation"] = generation;
	checkpoint["next_match"] = generation + 10;
	checkpoint["terminal_receipts"] = receipts;
	RoomAuthority full("Other");
	EXPECT(full.RestoreCheckpoint(checkpoint));
	EXPECT(FindMember(full.SnapshotView(), room.p1) != nullptr);

	const auto before = full.Checkpoint();
	const auto refused = KickBy(full, room.host, room.p1);
	EXPECT(!refused.accepted && refused.reason == RejectReason::TerminalLedgerFull);
	EXPECT(FindMember(full.SnapshotView(), room.p1) != nullptr && !full.SnapshotView().closed);
	EXPECT(full.KickedAccounts().empty());
	EXPECT(full.Checkpoint() == before);
	// The account still belongs to the member who is still here.
	const auto duplicate = full.Join("Dup", Peer(21), false, Profile("ember-p1"));
	EXPECT(!duplicate.accepted && duplicate.reason == RejectReason::NameTaken);

	// A kick that does remove its target still bans it, ledger full or not.
	EXPECT(KickBy(full, room.host, room.bystander).accepted);
	EXPECT(FindMember(full.SnapshotView(), room.bystander) == nullptr);
	EXPECT(full.KickedAccounts() == std::vector<std::string>{"ember-bystander"});
}

static void TestDuplicateAndMissingAccounts() {
	auto authority = NewRoom();
	EXPECT(JoinAs(authority, 1, "ember-a").accepted);
	EXPECT(!JoinAs(authority, 2, "ember-a").accepted);
	EXPECT(authority.SnapshotView().members.size() == 1);
	// The account is free again once its member has left.
	const auto member = authority.SnapshotView().members.front().id;
	EXPECT(authority.Leave(member).accepted);
	EXPECT(JoinAs(authority, 3, "ember-a").accepted);
	// A server-owned room has no account-less members.
	EXPECT(!JoinAs(authority, 4, "").accepted);
	EXPECT(!authority.Join("NoProfile", Peer(5)).accepted);
}

static void TestSetServerOwnedNeedsAFreshRoom() {
	RoomAuthority used("Used", 8, 1);
	Join(used, 0, true);
	EXPECT(!used.SetServerOwned() && !used.SnapshotView().serverOwned);
	RoomAuthority emptied("Emptied", 8, 1);
	const auto member = Join(emptied, 0, true);
	EXPECT(emptied.Leave(member).accepted);
	EXPECT(!emptied.SetServerOwned());
	// Only a player creates a Practice room, so none is ever a public one.
	RoomAuthority practice("Practice", 8, 1, Rules(), true);
	EXPECT(!practice.SetServerOwned() && !practice.SnapshotView().serverOwned);
	RoomAuthority fresh("Fresh", 8, 1);
	EXPECT(fresh.SetServerOwned() && !fresh.SnapshotView().practice);
}

// The delay policy is the room's own and not a table rule: a public room can be
// Own, an emptied room keeps it when its rules go back to the default, and it
// survives a checkpoint.
static void TestDelayPolicyIsKept() {
	RoomAuthority authority("Own", 8, 1, PublicRoomRules(), false, DelayPolicy::Own);
	EXPECT(authority.SetServerOwned() && authority.SnapshotView().serverOwned);
	EXPECT(authority.SnapshotView().delayPolicy == DelayPolicy::Own);
	const auto host = MemberOf(JoinAs(authority, 1, "ember-a"));
	auto rules = TableAction(authority, host, 0, ActionKind::SetRules);
	rules.rules.format = SetFormat::Unlimited;
	EXPECT(authority.Apply(host, rules).accepted);
	EXPECT(authority.Leave(host).accepted);
	// The rules went back to the public default; the policy did not move.
	EXPECT(authority.SnapshotView().tables[0].rules == PublicRoomRules());
	EXPECT(authority.SnapshotView().delayPolicy == DelayPolicy::Own);
	EXPECT(JoinAs(authority, 2, "ember-b").accepted && authority.SnapshotView().delayPolicy == DelayPolicy::Own);
	RoomAuthority restored("Other");
	EXPECT(restored.RestoreCheckpoint(authority.Checkpoint()));
	EXPECT(restored.SnapshotView().serverOwned && restored.SnapshotView().delayPolicy == DelayPolicy::Own);
	EXPECT(restored.Checkpoint() == authority.Checkpoint());
	// A Shared public room says nothing about it.
	auto shared = NewRoom();
	EXPECT(shared.SnapshotView().delayPolicy == DelayPolicy::Shared && !shared.Checkpoint().at("snapshot").contains("delay"));
}

static void TestCheckpointRoundTrip() {
	auto authority = NewRoom(7);
	const auto host = MemberOf(JoinAs(authority, 1, "ember-host"));
	const auto kicked = MemberOf(JoinAs(authority, 2, "ember-kicked"));
	EXPECT(JoinAs(authority, 3, "ember-stays").accepted);
	auto kick = TableAction(authority, host, 0, ActionKind::Kick);
	kick.target = kicked;
	EXPECT(authority.Apply(host, kick).accepted);
	const auto checkpoint = authority.Checkpoint();
	EXPECT(checkpoint.at("snapshot").value("server_owned", false));
	EXPECT(checkpoint.at("accounts").size() == 2 && checkpoint.at("kicked_accounts").size() == 1);
	RoomAuthority restored("Other");
	EXPECT(restored.RestoreCheckpoint(checkpoint));
	EXPECT(restored.SnapshotView().serverOwned && restored.KickedAccounts() == authority.KickedAccounts());
	EXPECT(restored.Checkpoint() == checkpoint);
	// The restored room still knows who is inside and who is out.
	EXPECT(!restored.Join("Dup", Peer(30), false, Profile("ember-stays")).accepted);
	const auto banned = restored.Join("Banned", Peer(31), false, Profile("ember-kicked"));
	EXPECT(!banned.accepted && banned.reason == RejectReason::MemberKicked);
	EXPECT(restored.Join("Fresh", Peer(32), false, Profile("ember-fresh")).accepted);

	// An empty server-owned room restores too, and stays open.
	auto empty = NewRoom(8);
	EXPECT(JoinAs(empty, 1, "ember-a").accepted);
	EXPECT(empty.Leave(empty.SnapshotView().members.front().id).accepted);
	RoomAuthority emptyRestored("Other");
	EXPECT(emptyRestored.RestoreCheckpoint(empty.Checkpoint()));
	EXPECT(emptyRestored.SnapshotView().host == 0 && !emptyRestored.SnapshotView().closed);

	// Checkpoint fields that do not fit are refused, not guessed at.
	auto missing = checkpoint;
	missing.erase("accounts");
	EXPECT(!RoomAuthority("Other").RestoreCheckpoint(missing));
	auto extra = checkpoint;
	extra["accounts"].push_back({{"member", 99}, {"account", "ember-ghost"}});
	EXPECT(!RoomAuthority("Other").RestoreCheckpoint(extra));
	auto duplicate = checkpoint;
	duplicate["accounts"][1]["account"] = duplicate["accounts"][0]["account"];
	EXPECT(!RoomAuthority("Other").RestoreCheckpoint(duplicate));
	auto wrongType = checkpoint;
	wrongType["snapshot"]["server_owned"] = "yes";
	EXPECT(!RoomAuthority("Other").RestoreCheckpoint(wrongType));
	auto privateWithAccounts = checkpoint;
	privateWithAccounts["snapshot"].erase("server_owned");
	EXPECT(!RoomAuthority("Other").RestoreCheckpoint(privateWithAccounts));
	// A public room is never a Practice room, also when its checkpoint says so;
	// the room it would restore into is left as it was.
	auto practice = checkpoint;
	practice["snapshot"]["practice"] = true;
	RoomAuthority untouched("Other");
	EXPECT(!untouched.RestoreCheckpoint(practice));
	EXPECT(!untouched.SnapshotView().practice && !untouched.SnapshotView().serverOwned && untouched.SnapshotView().members.empty());
}

// Both fighters take game `generation`'s terminal receipt, which frees the table.
static void Acknowledge(RoomAuthority& authority, std::uint64_t generation, MemberId p1, MemberId p2) {
	for (const auto member : {p1, p2}) {
		auto ack = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
		ack.matchGeneration = generation;
		EXPECT(authority.Apply(member, ack).accepted);
	}
}

// Readies the two seated fighters at table 0, plays one game to `result`, has
// both acknowledge it and returns its generation.
static std::uint64_t PlayGame(RoomAuthority& authority, MatchResult result) {
	const auto table = authority.SnapshotView().tables[0];
	for (const auto member : {table.p1, table.p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, table.p1, table.p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	EXPECT(authority.EndMatch(0, generation, result).accepted);
	Acknowledge(authority, generation, table.p1, table.p2);
	return generation;
}

// Games won between two accounts are kept for the skill rating, oldest first
// and at most MaximumWonGames; draws count for nothing, and the list survives a
// checkpoint that is refused when it does not fit.
static void TestWonGamesAreKeptByAccount() {
	MatchRoom room;
	StartMatchRoom(room);
	auto& authority = room.authority;
	const auto first = authority.SnapshotView().tables[0].matchGeneration;
	EXPECT(authority.EndMatch(0, first, MatchResult::P2Win).accepted);
	Acknowledge(authority, first, room.p1, room.p2);
	const std::vector<sf4e::WonGame> won{{0, first, "ember-p2", "ember-p1"}};
	EXPECT(authority.WonGames() == won);
	PlayGame(authority, MatchResult::Draw);
	EXPECT(authority.WonGames().size() == 1);
	std::uint64_t last = 0;
	for (int i = 0; i < 40; ++i) last = PlayGame(authority, i % 2 ? MatchResult::P1Win : MatchResult::P2Win);
	const auto games = authority.WonGames();
	EXPECT(games.size() == sf4e::MaximumWonGames && games.back().generation == last && games.front().generation > first);
	for (const auto& game : games) EXPECT(game.winner != game.loser);

	const auto checkpoint = authority.Checkpoint();
	RoomAuthority restored("Other");
	EXPECT(restored.RestoreCheckpoint(checkpoint) && restored.WonGames() == games);
	// A room from before won games restores with none.
	auto older = checkpoint;
	older.erase("won_games");
	RoomAuthority upgraded("Other");
	EXPECT(upgraded.RestoreCheckpoint(older) && upgraded.WonGames().empty());
	auto over = checkpoint;
	over["won_games"].push_back(over["won_games"][0]);
	EXPECT(!RoomAuthority("Other").RestoreCheckpoint(over));
	auto self = checkpoint;
	self["won_games"][0]["loser"] = self["won_games"][0]["winner"];
	EXPECT(!RoomAuthority("Other").RestoreCheckpoint(self));
	auto table = checkpoint;
	table["won_games"][0]["table"] = TableCount;
	EXPECT(!RoomAuthority("Other").RestoreCheckpoint(table));

	// A private room has no accounts and keeps no games.
	RoomAuthority privateRoom("Private");
	const auto a = MemberOf(privateRoom.Join("A", Peer(1), true));
	const auto b = MemberOf(privateRoom.Join("B", Peer(2)));
	for (const auto member : {a, b}) EXPECT(privateRoom.Apply(member, TableAction(privateRoom, member, 0, ActionKind::Queue)).accepted);
	PlayGame(privateRoom, MatchResult::P1Win);
	EXPECT(privateRoom.WonGames().empty() && !privateRoom.Checkpoint().contains("won_games"));
}

static bool Mentions(const nlohmann::json& value, const char* text) {
	return value.dump().find(text) != std::string::npos;
}

static void TestAccountsStayOutOfSnapshots() {
	auto authority = NewRoom();
	const auto host = MemberOf(JoinAs(authority, 1, "ember-secret-a"));
	const auto other = MemberOf(JoinAs(authority, 2, "ember-secret-b"));
	for (const auto member : {host, other}) {
		const auto snapshot = nlohmann::json(authority.SnapshotFor(member));
		EXPECT(!Mentions(snapshot, "ember-secret") && !Mentions(snapshot, "account"));
		EXPECT(snapshot.value("server_owned", false));
	}
	const auto joined = JoinAs(authority, 3, "ember-secret-c");
	EXPECT(!Mentions(nlohmann::json(joined), "ember-secret"));
	// A snapshot with the flag reads back with the flag.
	const auto round = nlohmann::json(authority.SnapshotFor(host)).get<Snapshot>();
	EXPECT(round.serverOwned);
	auto bad = nlohmann::json(authority.SnapshotFor(host));
	bad["server_owned"] = 1;
	bool threw = false;
	try { bad.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
	EXPECT(threw);
}

// A public room starts every table at first to 2, winner stays, and goes back
// to that when it empties, whatever its last host chose.
static void TestPublicRulesAndReopen() {
	RoomAuthority authority("Public", 8, 1, PublicRoomRules());
	EXPECT(authority.SetServerOwned());
	for (const auto& table : authority.SnapshotView().tables)
		EXPECT(table.rules.format == SetFormat::Ft2 && table.rules.rotation == RotationMode::WinnerStays);
	const auto host = MemberOf(JoinAs(authority, 1, "ember-a"));
	auto rules = TableAction(authority, host, 2, ActionKind::SetRules);
	rules.rules.format = SetFormat::Unlimited;
	rules.rules.rotation = RotationMode::BothRotate;
	EXPECT(authority.Apply(host, rules).accepted);
	EXPECT(authority.SnapshotView().tables[2].rules.format == SetFormat::Unlimited);
	const auto before = authority.SnapshotView().tables[2].revision;
	EXPECT(authority.Leave(host).accepted);
	const auto& snapshot = authority.SnapshotView();
	EXPECT(!snapshot.closed && snapshot.members.empty());
	for (const auto& table : snapshot.tables) EXPECT(table.rules == PublicRoomRules());
	EXPECT(snapshot.tables[2].revision > before);
	// The next joiner, now the host, finds the default.
	EXPECT(JoinAs(authority, 2, "ember-b").accepted && authority.SnapshotView().tables[2].rules == PublicRoomRules());
}

static void TestPrivateRoomsAreUnchanged() {
	RoomAuthority authority("Private", 8, 1);
	// A private room still needs its hosting player before anyone else.
	const auto early = authority.Join("Early", Peer(1), false, Profile(""));
	EXPECT(!early.accepted && early.reason == RejectReason::Unauthorized);
	const auto host = Join(authority, 0, true);
	const auto guest = Join(authority, 1);
	auto kick = TableAction(authority, host, 0, ActionKind::Kick);
	kick.target = guest;
	EXPECT(authority.Apply(host, kick).accepted);
	EXPECT(!authority.Join("Again", Peer(1)).accepted);
	EXPECT(authority.KickedAccounts().empty() && !authority.ServerOwned());
	const auto checkpoint = authority.Checkpoint();
	const auto snapshot = nlohmann::json(authority.SnapshotCopy());
	for (const char* key : {"server_owned", "account", "kicked_accounts"}) {
		EXPECT(!Mentions(checkpoint, key) && !Mentions(snapshot, key) && !Mentions(nlohmann::json(authority.SnapshotFor(host)), key));
	}
	// The host leaving an empty private room still closes it.
	EXPECT(authority.Leave(host).accepted && authority.SnapshotView().closed);
	// Its rules are the ones it was opened with.
	EXPECT(authority.SnapshotView().tables[0].rules == Rules());
	// And it cannot be switched over once people are in it.
	RoomAuthority used("Used", 8, 1);
	Join(used, 0, true);
	EXPECT(!used.SetServerOwned());
}

int main() {
	TestHostIsFirstJoinerThenOldest();
	TestEmptyRoomStaysOpen();
	TestKickKeysOnTheAccount();
	TestBansAreKeptThenTheKickAtTheCapClosesTheRoom();
	TestKickBansOnlyAfterTheMemberIsRemoved();
	TestDuplicateAndMissingAccounts();
	TestSetServerOwnedNeedsAFreshRoom();
	TestDelayPolicyIsKept();
	TestCheckpointRoundTrip();
	TestWonGamesAreKeptByAccount();
	TestAccountsStayOutOfSnapshots();
	TestPrivateRoomsAreUnchanged();
	TestPublicRulesAndReopen();

	if (TEST_FAILURES()) return 1;
	std::puts("RoomServerOwned test passed");
	return 0;
}
