#include "room_authority_support.hxx"

// A server-owned room: no player hosts it, the first joiner moderates, and an
// Ember ID (the account) is the identity a kick and a duplicate join key on.

static MemberProfile Profile(const std::string& account) {
	MemberProfile profile;
	profile.account = account;
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
	CHECK(authority.SetServerOwned());
	return authority;
}

static void TestHostIsFirstJoinerThenOldest() {
	auto authority = NewRoom();
	CHECK(authority.SnapshotView().serverOwned && authority.SnapshotView().host == 0);
	const auto first = JoinAs(authority, 1, "ember-a");
	CHECK(first.accepted);
	const auto a = MemberOf(first);
	CHECK(authority.SnapshotView().host == a && FindMember(authority.SnapshotView(), a)->host);
	const auto b = MemberOf(JoinAs(authority, 2, "ember-b"));
	const auto c = MemberOf(JoinAs(authority, 3, "ember-c"));
	CHECK(b && c && authority.SnapshotView().host == a && !FindMember(authority.SnapshotView(), b)->host);
	// The host argument means nothing here: a second "host" is an ordinary member.
	const auto claimed = authority.Join("Claimer", Peer(4), true, Profile("ember-d"));
	CHECK(claimed.accepted && authority.SnapshotView().host == a && !claimed.snapshot.members.back().host);
	CHECK(authority.Leave(a).accepted);
	CHECK(authority.SnapshotView().host == b && FindMember(authority.SnapshotView(), b)->host);
	CHECK(!FindMember(authority.SnapshotView(), c)->host);
}

static void TestEmptyRoomStaysOpen() {
	auto authority = NewRoom();
	const auto a = MemberOf(JoinAs(authority, 1, "ember-a"));
	const auto b = MemberOf(JoinAs(authority, 2, "ember-b"));
	CHECK(authority.Apply(a, [&] {
		auto lock = TableAction(authority, a, 0, ActionKind::Lock);
		lock.locked = true;
		return lock;
	}()).accepted);
	CHECK(authority.SnapshotView().locked);
	// Seat both, so leaving also has to free the table.
	CHECK(authority.Apply(a, TableAction(authority, a, 0, ActionKind::Queue)).accepted);
	CHECK(authority.Apply(b, TableAction(authority, b, 0, ActionKind::Queue)).accepted);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Waiting);
	CHECK(authority.Leave(a).accepted);
	const auto last = authority.Leave(b);
	CHECK(last.accepted);
	for (const auto& event : last.events) CHECK(event.kind != Event::Kind::RoomClosed);
	const auto& snapshot = authority.SnapshotView();
	CHECK(!snapshot.closed && !snapshot.locked && snapshot.host == 0 && snapshot.members.empty() && snapshot.serverOwned);
	CHECK(snapshot.tables[0].phase == TablePhase::Idle && snapshot.tables[0].p1 == 0 && snapshot.tables[0].p2 == 0);
	CHECK(authority.KickedAccounts().empty());
	const auto next = JoinAs(authority, 3, "ember-c");
	CHECK(next.accepted && authority.SnapshotView().host == MemberOf(next));
	CHECK(FindMember(authority.SnapshotView(), MemberOf(next))->host);
}

static void TestKickKeysOnTheAccount() {
	auto authority = NewRoom();
	const auto host = MemberOf(JoinAs(authority, 1, "ember-host"));
	const auto target = MemberOf(JoinAs(authority, 2, "ember-bad"));
	auto kick = TableAction(authority, host, 0, ActionKind::Kick);
	kick.target = target;
	CHECK(authority.Apply(host, kick).accepted);
	CHECK(authority.KickedAccounts() == std::vector<std::string>{"ember-bad"});
	// A new connection with the same account is still refused.
	const auto again = authority.Join("Back", Peer(20), false, Profile("ember-bad"));
	CHECK(!again.accepted && again.reason == RejectReason::MemberKicked);
	// Someone else on the kicked member's old connection is not.
	const auto other = authority.Join("Other", Peer(2), false, Profile("ember-good"));
	CHECK(other.accepted);
	CHECK(authority.SnapshotView().members.size() == 2);
}

// Bans are never evicted: they are kept up to the lifetime cap, and the kick
// that would exceed it removes its target and closes the room instead.
static void TestBansAreKeptThenTheKickAtTheCapClosesTheRoom() {
	auto authority = NewRoom();
	const auto host = MemberOf(JoinAs(authority, 1, "ember-host"));
	const auto account = [](std::size_t i) { return "ember-banned-" + std::to_string(i); };
	const auto kickNext = [&](std::size_t i) {
		const auto target = MemberOf(JoinAs(authority, 100 + static_cast<int>(i), account(i)));
		CHECK(target != 0);
		auto kick = TableAction(authority, host, 0, ActionKind::Kick);
		kick.target = target;
		return std::make_pair(target, authority.Apply(host, kick));
	};
	for (std::size_t i = 0; i < MaximumKickedAccounts; ++i) {
		const auto kicked = kickNext(i);
		CHECK(kicked.second.accepted && !authority.SnapshotView().closed);
	}
	CHECK(authority.KickedAccounts().size() == MaximumKickedAccounts);
	// Nothing was evicted: the first and the last ban still refuse a rejoin.
	for (const auto index : {std::size_t(0), MaximumKickedAccounts - 1}) {
		const auto again = authority.Join("Back", Peer(9000), false, Profile(account(index)));
		CHECK(!again.accepted && again.reason == RejectReason::MemberKicked);
	}
	// The next kick removes the member, records no ban and closes the room.
	const auto last = kickNext(MaximumKickedAccounts);
	CHECK(last.second.accepted && authority.SnapshotView().closed);
	CHECK(FindMember(authority.SnapshotView(), last.first) == nullptr);
	CHECK(authority.KickedAccounts().size() == MaximumKickedAccounts);
	const auto has = [&](Event::Kind kind) {
		return std::any_of(last.second.events.begin(), last.second.events.end(), [kind](const Event& event) { return event.kind == kind; });
	};
	CHECK(has(Event::Kind::MemberRemoved) && has(Event::Kind::RoomClosed));
	CHECK(last.second.events.back().kind == Event::Kind::RoomClosed && last.second.snapshot.closed);
	for (const auto& table : authority.SnapshotView().tables) CHECK(table.phase == TablePhase::Closed);
	CHECK(!authority.Join("Late", Peer(9001), false, Profile("ember-late")).accepted);
	// The closed room's checkpoint restores; one ban over the cap does not.
	const auto checkpoint = authority.Checkpoint();
	RoomAuthority restored("Other");
	CHECK(restored.RestoreCheckpoint(checkpoint) && restored.KickedAccounts() == authority.KickedAccounts());
	auto over = checkpoint;
	over["kicked_accounts"].push_back("ember-one-too-many");
	CHECK(!RoomAuthority("Other").RestoreCheckpoint(over));
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
	CHECK(room.host && room.p1 && room.p2 && room.bystander);
	for (const auto member : {room.p1, room.p2}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	for (const auto member : {room.p1, room.p2}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(0, room.p1, room.p2).accepted);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Playing);
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
		CHECK(KickBy(authority, room.host, room.p1).accepted);
		CHECK(FindMember(authority.SnapshotView(), room.p1) == nullptr && FindMember(authority.SnapshotView(), room.p2) != nullptr);
		CHECK(authority.KickedAccounts() == std::vector<std::string>{"ember-p1"});
		CHECK(!authority.Join("Back", Peer(20), false, Profile("ember-p1")).accepted);
	}

	// A full ledger: restore the room with the maximum number of unacknowledged
	// receipts on the idle tables, which is the only way a kick's Leave refuses.
	MatchRoom room;
	StartMatchRoom(room);
	auto& authority = room.authority;
	auto checkpoint = authority.Checkpoint();
	auto recipients = checkpoint.at("active_match_recipients").at(0);
	CHECK(recipients.is_array() && recipients.size() == 2);
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
	CHECK(full.RestoreCheckpoint(checkpoint));
	CHECK(FindMember(full.SnapshotView(), room.p1) != nullptr);

	const auto before = full.Checkpoint();
	const auto refused = KickBy(full, room.host, room.p1);
	CHECK(!refused.accepted && refused.reason == RejectReason::TerminalLedgerFull);
	CHECK(FindMember(full.SnapshotView(), room.p1) != nullptr && !full.SnapshotView().closed);
	CHECK(full.KickedAccounts().empty());
	CHECK(full.Checkpoint() == before);
	// The account still belongs to the member who is still here.
	const auto duplicate = full.Join("Dup", Peer(21), false, Profile("ember-p1"));
	CHECK(!duplicate.accepted && duplicate.reason == RejectReason::NameTaken);

	// A kick that does remove its target still bans it, ledger full or not.
	CHECK(KickBy(full, room.host, room.bystander).accepted);
	CHECK(FindMember(full.SnapshotView(), room.bystander) == nullptr);
	CHECK(full.KickedAccounts() == std::vector<std::string>{"ember-bystander"});
}

static void TestDuplicateAndMissingAccounts() {
	auto authority = NewRoom();
	CHECK(JoinAs(authority, 1, "ember-a").accepted);
	CHECK(!JoinAs(authority, 2, "ember-a").accepted);
	CHECK(authority.SnapshotView().members.size() == 1);
	// The account is free again once its member has left.
	const auto member = authority.SnapshotView().members.front().id;
	CHECK(authority.Leave(member).accepted);
	CHECK(JoinAs(authority, 3, "ember-a").accepted);
	// A server-owned room has no account-less members.
	CHECK(!JoinAs(authority, 4, "").accepted);
	CHECK(!authority.Join("NoProfile", Peer(5)).accepted);
}

static void TestSetServerOwnedNeedsAFreshRoom() {
	RoomAuthority used("Used", 8, 1);
	Join(used, 0, true);
	CHECK(!used.SetServerOwned() && !used.SnapshotView().serverOwned);
	RoomAuthority emptied("Emptied", 8, 1);
	const auto member = Join(emptied, 0, true);
	CHECK(emptied.Leave(member).accepted);
	CHECK(!emptied.SetServerOwned());
}

static void TestCheckpointRoundTrip() {
	auto authority = NewRoom(7);
	const auto host = MemberOf(JoinAs(authority, 1, "ember-host"));
	const auto kicked = MemberOf(JoinAs(authority, 2, "ember-kicked"));
	CHECK(JoinAs(authority, 3, "ember-stays").accepted);
	auto kick = TableAction(authority, host, 0, ActionKind::Kick);
	kick.target = kicked;
	CHECK(authority.Apply(host, kick).accepted);
	const auto checkpoint = authority.Checkpoint();
	CHECK(checkpoint.at("snapshot").value("server_owned", false));
	CHECK(checkpoint.at("accounts").size() == 2 && checkpoint.at("kicked_accounts").size() == 1);
	RoomAuthority restored("Other");
	CHECK(restored.RestoreCheckpoint(checkpoint));
	CHECK(restored.SnapshotView().serverOwned && restored.KickedAccounts() == authority.KickedAccounts());
	CHECK(restored.Checkpoint() == checkpoint);
	// The restored room still knows who is inside and who is out.
	CHECK(!restored.Join("Dup", Peer(30), false, Profile("ember-stays")).accepted);
	const auto banned = restored.Join("Banned", Peer(31), false, Profile("ember-kicked"));
	CHECK(!banned.accepted && banned.reason == RejectReason::MemberKicked);
	CHECK(restored.Join("Fresh", Peer(32), false, Profile("ember-fresh")).accepted);

	// An empty server-owned room restores too, and stays open.
	auto empty = NewRoom(8);
	CHECK(JoinAs(empty, 1, "ember-a").accepted);
	CHECK(empty.Leave(empty.SnapshotView().members.front().id).accepted);
	RoomAuthority emptyRestored("Other");
	CHECK(emptyRestored.RestoreCheckpoint(empty.Checkpoint()));
	CHECK(emptyRestored.SnapshotView().host == 0 && !emptyRestored.SnapshotView().closed);

	// Checkpoint fields that do not fit are refused, not guessed at.
	auto missing = checkpoint;
	missing.erase("accounts");
	CHECK(!RoomAuthority("Other").RestoreCheckpoint(missing));
	auto extra = checkpoint;
	extra["accounts"].push_back({{"member", 99}, {"account", "ember-ghost"}});
	CHECK(!RoomAuthority("Other").RestoreCheckpoint(extra));
	auto duplicate = checkpoint;
	duplicate["accounts"][1]["account"] = duplicate["accounts"][0]["account"];
	CHECK(!RoomAuthority("Other").RestoreCheckpoint(duplicate));
	auto wrongType = checkpoint;
	wrongType["snapshot"]["server_owned"] = "yes";
	CHECK(!RoomAuthority("Other").RestoreCheckpoint(wrongType));
	auto privateWithAccounts = checkpoint;
	privateWithAccounts["snapshot"].erase("server_owned");
	CHECK(!RoomAuthority("Other").RestoreCheckpoint(privateWithAccounts));
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
		CHECK(!Mentions(snapshot, "ember-secret") && !Mentions(snapshot, "account"));
		CHECK(snapshot.value("server_owned", false));
	}
	const auto joined = JoinAs(authority, 3, "ember-secret-c");
	CHECK(!Mentions(nlohmann::json(joined), "ember-secret"));
	// A snapshot with the flag reads back with the flag.
	const auto round = nlohmann::json(authority.SnapshotFor(host)).get<Snapshot>();
	CHECK(round.serverOwned);
	auto bad = nlohmann::json(authority.SnapshotFor(host));
	bad["server_owned"] = 1;
	bool threw = false;
	try { bad.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
	CHECK(threw);
}

// A public room starts every table at first to 2, winner stays, and goes back
// to that when it empties, whatever its last host chose.
static void TestPublicRulesAndReopen() {
	RoomAuthority authority("Public", 8, 1, PublicRoomRules());
	CHECK(authority.SetServerOwned());
	for (const auto& table : authority.SnapshotView().tables)
		CHECK(table.rules.format == SetFormat::Ft2 && table.rules.rotation == RotationMode::WinnerStays);
	const auto host = MemberOf(JoinAs(authority, 1, "ember-a"));
	auto rules = TableAction(authority, host, 2, ActionKind::SetRules);
	rules.rules.format = SetFormat::Unlimited;
	rules.rules.rotation = RotationMode::BothRotate;
	CHECK(authority.Apply(host, rules).accepted);
	CHECK(authority.SnapshotView().tables[2].rules.format == SetFormat::Unlimited);
	const auto before = authority.SnapshotView().tables[2].revision;
	CHECK(authority.Leave(host).accepted);
	const auto& snapshot = authority.SnapshotView();
	CHECK(!snapshot.closed && snapshot.members.empty());
	for (const auto& table : snapshot.tables) CHECK(table.rules == PublicRoomRules());
	CHECK(snapshot.tables[2].revision > before);
	// The next joiner, now the host, finds the default.
	CHECK(JoinAs(authority, 2, "ember-b").accepted && authority.SnapshotView().tables[2].rules == PublicRoomRules());
}

static void TestPrivateRoomsAreUnchanged() {
	RoomAuthority authority("Private", 8, 1);
	// A private room still needs its hosting player before anyone else.
	const auto early = authority.Join("Early", Peer(1), false, Profile(""));
	CHECK(!early.accepted && early.reason == RejectReason::Unauthorized);
	const auto host = Join(authority, 0, true);
	const auto guest = Join(authority, 1);
	auto kick = TableAction(authority, host, 0, ActionKind::Kick);
	kick.target = guest;
	CHECK(authority.Apply(host, kick).accepted);
	CHECK(!authority.Join("Again", Peer(1)).accepted);
	CHECK(authority.KickedAccounts().empty() && !authority.ServerOwned());
	const auto checkpoint = authority.Checkpoint();
	const auto snapshot = nlohmann::json(authority.SnapshotCopy());
	for (const char* key : {"server_owned", "account", "kicked_accounts"}) {
		CHECK(!Mentions(checkpoint, key) && !Mentions(snapshot, key) && !Mentions(nlohmann::json(authority.SnapshotFor(host)), key));
	}
	// The host leaving an empty private room still closes it.
	CHECK(authority.Leave(host).accepted && authority.SnapshotView().closed);
	// Its rules are the ones it was opened with.
	CHECK(authority.SnapshotView().tables[0].rules == Rules());
	// And it cannot be switched over once people are in it.
	RoomAuthority used("Used", 8, 1);
	Join(used, 0, true);
	CHECK(!used.SetServerOwned());
}

int main() {
	TestHostIsFirstJoinerThenOldest();
	TestEmptyRoomStaysOpen();
	TestKickKeysOnTheAccount();
	TestBansAreKeptThenTheKickAtTheCapClosesTheRoom();
	TestKickBansOnlyAfterTheMemberIsRemoved();
	TestDuplicateAndMissingAccounts();
	TestSetServerOwnedNeedsAFreshRoom();
	TestCheckpointRoundTrip();
	TestAccountsStayOutOfSnapshots();
	TestPrivateRoomsAreUnchanged();
	TestPublicRulesAndReopen();

	if (failures) return 1;
	std::puts("RoomServerOwned test passed");
	return 0;
}
