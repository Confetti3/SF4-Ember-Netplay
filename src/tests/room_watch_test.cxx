// A room bound to a tournament match that names watchers: they join as
// spectators and only watch (no table action, chat, host power or vote in
// anything), and none of their coming and going moves table 0's revision, so
// a fighter's Ready is never made stale by one.
#include "room_authority_support.hxx"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// 64 lowercase hex digits of one digit: 'a' and 'b' are the fighters, '1' up the watchers.
static std::string Endpoint(char digit) { return std::string(64, digit); }
static std::string WatcherEndpoint(int index) { return Endpoint(static_cast<char>('0' + index)); }

static TournamentBinding Binding(std::uint64_t revision, const std::vector<int>& watchers, int gamesToWin = 2) {
	TournamentBinding binding;
	binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	binding.assignmentGeneration = 1;
	binding.bindingRevision = revision;
	binding.gamesToWin = static_cast<std::uint8_t>(gamesToWin);
	binding.fighters[0] = {Endpoint('a'), "emb1_player1"};
	binding.fighters[1] = {Endpoint('b'), "emb1_player2"};
	for (const int index : watchers) binding.watchers.push_back({WatcherEndpoint(index), "emb1_watcher" + std::to_string(index)});
	return binding;
}

static MemberProfile Observer() {
	MemberProfile profile;
	profile.observer = true;
	return profile;
}

static MemberId Enter(RoomAuthority& authority, const std::string& name, const std::string& endpoint, bool host, MemberProfile profile) {
	const auto result = authority.Join(name, ConnectionRef{"room", endpoint}, host, profile);
	if (!result.accepted) return 0;
	for (const auto& member : result.snapshot.members)
		if (member.connection.user == endpoint) return member.id;
	return 0;
}
static MemberId Fighter(RoomAuthority& authority, char digit, bool host = false) {
	return Enter(authority, digit == 'a' ? "Player 1" : "Player 2", Endpoint(digit), host, {});
}
static MemberId Watcher(RoomAuthority& authority, int index) {
	return Enter(authority, "Watcher " + std::to_string(index), WatcherEndpoint(index), false, Observer());
}

static bool Listed(const std::vector<MemberId>& list, MemberId member) {
	return std::find(list.begin(), list.end(), member) != list.end();
}

static bool HasEvent(const Result& result, Event::Kind kind) {
	return std::any_of(result.events.begin(), result.events.end(), [kind](const Event& event) { return event.kind == kind; });
}

static Action RoomAction(const RoomAuthority& authority, MemberId member, ActionKind kind) {
	Action action = TableAction(authority, member, TournamentTable, kind);
	action.revision = authority.SnapshotView().revision;
	return action;
}

static Action Permit(const RoomAuthority& authority, MemberId member, const std::string& permit) {
	Action action = TableAction(authority, member, TournamentTable, ActionKind::PermitReady);
	action.matchGeneration = authority.SnapshotView().tables[TournamentTable].permitGeneration;
	action.text = permit;
	action.startWindowMs = PermitStartMs;
	return action;
}

// The fighters' room bound to a match naming `watchers`, none of them in yet.
struct Match {
	RoomAuthority authority{"Match", 8, 1};
	MemberId a = 0, b = 0;
	explicit Match(const std::vector<int>& watchers = {}) {
		authority.AdvanceTime(1000);
		a = Fighter(authority, 'a', true);
		CHECK(authority.BindTournament(Binding(1, watchers)).accepted);
		b = Fighter(authority, 'b');
	}
	const Snapshot& View() const { return authority.SnapshotView(); }
	const Table& Table0() const { return View().tables[TournamentTable]; }
	const Member* Find(MemberId member) const { return FindMember(View(), member); }
	bool Spectates(MemberId member) const { return Listed(Table0().spectators, member); }
	bool WatchesNext(MemberId member) const { return Listed(Table0().watchingNext, member); }
	// Both fighters ready, and both holding the bridge's permit: the game may begin.
	void ReadyBoth() {
		for (const auto member : {a, b}) CHECK(authority.Apply(member, TableAction(authority, member, TournamentTable, ActionKind::Ready)).accepted);
	}
	void PermitBoth() {
		CHECK(authority.Apply(a, Permit(authority, a, "per_one")).accepted);
		CHECK(authority.Apply(b, Permit(authority, b, "per_one")).accepted);
	}
	std::uint64_t Begin() {
		ReadyBoth();
		PermitBoth();
		CHECK(authority.BeginMatch(TournamentTable, a, b).accepted);
		return Table0().matchGeneration;
	}
	// The game ends and every recipient, watchers included, acknowledges it.
	void End(std::uint64_t generation) {
		CHECK(authority.EndMatch(TournamentTable, generation, MatchResult::P1Win).accepted);
		for (const auto member : authority.TerminalMembers(TournamentTable, generation)) {
			Action acknowledgment = TableAction(authority, member, TournamentTable, ActionKind::AcknowledgeTerminal);
			acknowledgment.matchGeneration = generation;
			CHECK(authority.Apply(member, acknowledgment).accepted);
		}
	}
};

// A bind that names watchers keeps them; a rebind without one sends it away;
// an equal binding that names other watchers is not a no-op; capacity stays.
static void TestBindNamesWatchers() {
	Match match;
	CHECK(!Watcher(match.authority, 1));
	CHECK(match.authority.BindTournament(Binding(2, {1, 2})).accepted);
	const MemberId first = Watcher(match.authority, 1), second = Watcher(match.authority, 2);
	CHECK(first && second && match.Spectates(first) && match.Spectates(second));
	CHECK(match.Find(first)->status == MemberStatus::WatchingNext);
	const auto capacity = match.View().capacity;
	// The same revision with one watcher fewer is a new binding, not the one held.
	const auto dropped = match.authority.BindTournament(Binding(2, {1}));
	CHECK(dropped.accepted && HasEvent(dropped, Event::Kind::MemberRemoved));
	CHECK(!match.Find(second) && match.Find(first) && match.View().tournament.watchers.size() == 1);
	// The identical binding changes nothing.
	const auto revision = match.View().revision;
	CHECK(match.authority.BindTournament(Binding(2, {1})).accepted && match.View().revision == revision);
	// A rebind that names none sends the last watcher away; the fighters stay.
	const auto gone = match.authority.BindTournament(Binding(3, {}));
	CHECK(gone.accepted && HasEvent(gone, Event::Kind::MemberRemoved) && !match.Find(first));
	CHECK(match.Find(match.a) && match.Find(match.b) && match.View().capacity == capacity);
	// An older binding for the match is still refused.
	CHECK(!match.authority.BindTournament(Binding(1, {1})).accepted);
}

// A named watcher bound into a room already holding it as a plain member (a
// modified game that left the observer flag out, and took a place) is moved
// to the spectators; one that already watches is left alone.
static void TestBindMovesAModifiedWatcher() {
	RoomAuthority authority("Match", 8, 1);
	const MemberId a = Fighter(authority, 'a', true);
	const MemberId b = Fighter(authority, 'b');
	const MemberId plain = Enter(authority, "Watcher 1", WatcherEndpoint(1), false, {});
	CHECK(a && b && plain);
	for (const auto member : {a, b, plain}) CHECK(authority.Apply(member, TableAction(authority, member, TournamentTable, ActionKind::Queue)).accepted);
	CHECK(authority.SnapshotView().tables[0].queue.size() == 1 && authority.SnapshotView().tables[0].queue[0] == plain);
	CHECK(authority.BindTournament(Binding(1, {1})).accepted);
	const auto& table = authority.SnapshotView().tables[0];
	CHECK(table.queue.empty() && table.p1 == a && table.p2 == b && Listed(table.spectators, plain));
	CHECK(FindMember(authority.SnapshotView(), plain)->status == MemberStatus::WatchingNext);
	// A watcher that took a seat is moved out of it, and the fighter the binding names gets it.
	RoomAuthority seated("Match", 8, 1);
	const MemberId host = Fighter(seated, 'a', true);
	const MemberId intruder = Enter(seated, "Watcher 1", WatcherEndpoint(1), false, {});
	for (const auto member : {host, intruder}) CHECK(seated.Apply(member, TableAction(seated, member, TournamentTable, ActionKind::Queue)).accepted);
	CHECK(seated.SnapshotView().tables[0].p2 == intruder);
	CHECK(seated.BindTournament(Binding(1, {1})).accepted);
	CHECK(seated.SnapshotView().tables[0].p2 == 0 && Listed(seated.SnapshotView().tables[0].spectators, intruder));
	const MemberId fighterB = Fighter(seated, 'b');
	CHECK(fighterB && seated.SnapshotView().tables[0].p1 == host && seated.SnapshotView().tables[0].p2 == fighterB);
	CHECK(Listed(seated.SnapshotView().tables[0].spectators, intruder));
	// A cooperative watcher already spectates, so a rebind that keeps it touches nothing at the table.
	Match match({1});
	Watcher(match.authority, 1);
	const auto revision = match.Table0().revision;
	CHECK(match.authority.BindTournament(Binding(2, {1})).accepted && match.Table0().revision == revision);
}

// An observer is refused where the room does not name its endpoint, and a
// named member comes in through a lock and over a small capacity, as a
// spectator or watching the next game, while a stranger still cannot.
static void TestJoin() {
	RoomAuthority casual("Casual", 8, 1);
	Fighter(casual, 'a', true);
	const auto refused = casual.Join("Watcher 1", ConnectionRef{"room", WatcherEndpoint(1)}, false, Observer());
	CHECK(!refused.accepted && refused.reason == RejectReason::Unauthorized);
	Match match({1});
	const auto unnamed = match.authority.Join("Watcher 2", ConnectionRef{"room", WatcherEndpoint(2)}, false, Observer());
	CHECK(!unnamed.accepted && unnamed.reason == RejectReason::Unauthorized);
	// A fighter that sets the flag is not a watcher; without it, it returns.
	CHECK(match.authority.Leave(match.b).accepted);
	const auto flagged = match.authority.Join("Player 2", ConnectionRef{"room", Endpoint('b')}, false, Observer());
	CHECK(!flagged.accepted && flagged.reason == RejectReason::Unauthorized);
	match.b = Fighter(match.authority, 'b');
	CHECK(match.b);
	// Locked and full to its two fighters, the room still takes its named watcher.
	auto lock = RoomAction(match.authority, match.a, ActionKind::Lock);
	lock.locked = true;
	CHECK(match.authority.Apply(match.a, lock).accepted);
	auto size = RoomAction(match.authority, match.a, ActionKind::SetCapacity);
	size.capacity = 2;
	CHECK(match.authority.Apply(match.a, size).accepted);
	const auto stranger = match.authority.Join("Player 3", ConnectionRef{"room", Endpoint('c')}, false, {});
	CHECK(!stranger.accepted && stranger.reason == RejectReason::RoomFull);
	const MemberId watcher = Watcher(match.authority, 1);
	CHECK(watcher && match.Spectates(watcher) && match.View().capacity == 2 && match.View().members.size() == 3);
	// Mid-game it watches the next game, and joins the spectators when this one closes.
	CHECK(match.authority.Leave(watcher).accepted);
	const auto generation = match.Begin();
	const MemberId late = Watcher(match.authority, 1);
	CHECK(late && match.WatchesNext(late) && !match.Spectates(late));
	CHECK(match.Find(late)->status == MemberStatus::WatchingNext);
	match.End(generation);
	CHECK(match.Spectates(late) && !match.WatchesNext(late));
}

// A fighter returns into a room that is full of watchers.
static void TestFighterReturnsToACrowd() {
	Match match({1, 2, 3, 4, 5, 6, 7, 8});
	for (int index = 1; index <= 8; ++index) CHECK(Watcher(match.authority, index));
	CHECK(match.View().members.size() == 10 && match.View().capacity == 8);
	CHECK(match.authority.Leave(match.b).accepted);
	const MemberId back = Fighter(match.authority, 'b');
	CHECK(back && match.Table0().p2 == back && match.View().members.size() == 10);
	CHECK(WatcherCount(match.View()) == 8 && !IsWatcher(match.View(), back) && !IsWatcher(match.View(), match.a));
	// More members than its capacity is a room a new leader restores and a member reads.
	RoomAuthority restored("Other", 8, 1);
	CHECK(restored.RestoreCheckpoint(match.authority.Checkpoint()) && restored.SnapshotView().members.size() == 10);
	Snapshot wire;
	nlohmann::json(match.View()).get_to(wire);
	CHECK(wire.members.size() == 10 && wire.capacity == 8 && wire.tournament.watchers.size() == 8);
}

// A watcher has no table action, no chat and no lock-in; it can only leave.
static void TestWatchersTakeNoAction() {
	Match match({1});
	const MemberId w = Watcher(match.authority, 1);
	for (const auto kind : {ActionKind::Watch, ActionKind::Unwatch, ActionKind::Queue, ActionKind::Unqueue, ActionKind::SetRules}) {
		const auto result = match.authority.Apply(w, TableAction(match.authority, w, TournamentTable, kind));
		CHECK(!result.accepted && result.reason == RejectReason::Unauthorized);
	}
	// Not at the other tables either, where a watcher would give up its place at this one.
	for (const auto kind : {ActionKind::Watch, ActionKind::Queue, ActionKind::Unqueue, ActionKind::Unwatch, ActionKind::SetRules}) {
		const auto result = match.authority.Apply(w, TableAction(match.authority, w, 1, kind));
		CHECK(!result.accepted && result.reason == RejectReason::Unauthorized && match.Spectates(w));
	}
	const auto locked = match.authority.Apply(w, TableAction(match.authority, w, TournamentTable, ActionKind::LockSpectating));
	CHECK(!locked.accepted && locked.reason == RejectReason::Unauthorized && !match.Find(w)->spectatorLocked);
	Action chat = RoomAction(match.authority, w, ActionKind::Chat);
	chat.text = "hello";
	const auto spoken = match.authority.Apply(w, chat);
	CHECK(!spoken.accepted && spoken.reason == RejectReason::Unauthorized && match.View().chat.empty());
	// A fighter chats, and the watcher sees it.
	Action reply = RoomAction(match.authority, match.a, ActionKind::Chat);
	reply.text = "hello";
	CHECK(match.authority.Apply(match.a, reply).accepted && match.View().chat.size() == 1);
	// The watcher can only leave; a game never waits on it.
	const auto generation = match.Begin();
	// A stream P1 dropped keeps its place for the next game: the runtime's leave of this one
	// is taken, changes nothing, and the watcher goes on to the next.
	Action dropped = TableAction(match.authority, w, TournamentTable, ActionKind::Unwatch);
	dropped.matchGeneration = generation;
	dropped.keepWatching = true;
	const auto tableRevision = match.Table0().revision;
	CHECK(match.authority.Apply(w, dropped).accepted && match.Spectates(w) && match.Table0().revision == tableRevision);
	// Without keepWatching it is Stop watching, which a watcher does not have.
	Action stopped = TableAction(match.authority, w, TournamentTable, ActionKind::Unwatch);
	stopped.matchGeneration = generation;
	// The leave changed nothing, so the id the room would stamp is the one just used: a new one.
	stopped.actionId = dropped.actionId + 1;
	CHECK(!match.authority.Apply(w, stopped).accepted && match.Spectates(w));
	match.End(generation);
	match.ReadyBoth();
	CHECK(match.Table0().phase == TablePhase::Ready && !match.Table0().spectatorHold && PermitPending(match.Table0()));
	CHECK(match.authority.Apply(w, TableAction(match.authority, w, TournamentTable, ActionKind::Leave)).accepted);
	CHECK(!match.Find(w) && !match.Spectates(w));
}

// Watchers coming, going and being sent away never move table 0's revision, so
// a Ready stamped before any of it is accepted the first time; a change to
// the set itself does move it.
static void TestReadyUnderChurn() {
	Match match({1, 2, 3});
	const MemberId first = Watcher(match.authority, 1);
	CHECK(first);
	CHECK(match.authority.Apply(match.a, TableAction(match.authority, match.a, TournamentTable, ActionKind::Ready)).accepted);
	const Action ready = TableAction(match.authority, match.b, TournamentTable, ActionKind::Ready);
	const auto tableRevision = match.Table0().revision;
	const auto roomRevision = match.View().revision;
	// A watcher joins, another leaves, a bind adds one, and another is revoked.
	const MemberId second = Watcher(match.authority, 2);
	CHECK(second && match.authority.Leave(first).accepted);
	CHECK(match.authority.BindTournament(Binding(2, {2, 3})).accepted);
	const MemberId third = Watcher(match.authority, 3);
	CHECK(third);
	const auto revoked = match.authority.BindTournament(Binding(3, {3}));
	CHECK(revoked.accepted && HasEvent(revoked, Event::Kind::MemberRemoved) && !match.Find(second));
	CHECK(match.Table0().revision == tableRevision && match.View().revision > roomRevision);
	const auto accepted = match.authority.Apply(match.b, ready);
	CHECK(accepted.accepted && match.Table0().phase == TablePhase::Ready);
	// A rebind that changes the set length or a fighter does move it, and a Ready stamped before is stale.
	for (const bool otherLength : {true, false}) {
		Match changed;
		const Action stamped = TableAction(changed.authority, changed.a, TournamentTable, ActionKind::Ready);
		auto rebind = Binding(2, {}, otherLength ? 3 : 2);
		if (!otherLength) rebind.fighters[1].endpoint = Endpoint('c');
		CHECK(changed.authority.BindTournament(rebind).accepted);
		const auto stale = changed.authority.Apply(changed.a, stamped);
		CHECK(!stale.accepted && stale.reason == RejectReason::StaleTable);
	}
}

// The spectators a game starts with are the list as it stands when it begins:
// a watcher placed before is in the roster, one that left before is not, and
// neither moved the table's revision.
static void TestSpectatorListAtStart() {
	Match match({1, 2});
	const MemberId stays = Watcher(match.authority, 1);
	match.ReadyBoth();
	match.PermitBoth();
	const auto tableRevision = match.Table0().revision;
	const MemberId leaves = Watcher(match.authority, 2);
	CHECK(leaves && match.authority.Leave(leaves).accepted);
	CHECK(match.Table0().revision == tableRevision);
	CHECK(match.authority.BeginMatch(TournamentTable, match.a, match.b).accepted);
	const auto roster = match.authority.LiveMatchRoster(TournamentTable);
	CHECK(roster.size() == 3 && roster[0] == match.a && roster[1] == match.b && Listed(roster, stays));
	CHECK(match.Find(stays)->status == MemberStatus::Watching);
}

// A watcher never holds host powers, even holding the host field when no
// fighter is left, and a fighter back in takes the field again.
static void TestHostStaysWithTheFighters() {
	RoomAuthority authority("Match", 8, 1);
	const MemberId a = Fighter(authority, 'a', true);
	CHECK(authority.BindTournament(Binding(1, {1})).accepted);
	const MemberId w = Watcher(authority, 1);
	const MemberId b = Fighter(authority, 'b');
	CHECK(a && w && b);
	// Host cannot be handed to a watcher.
	auto transfer = RoomAction(authority, a, ActionKind::TransferHost);
	transfer.target = w;
	const auto refused = authority.Apply(a, transfer);
	CHECK(!refused.accepted && refused.reason == RejectReason::Unauthorized && authority.SnapshotView().host == a);
	// The host leaves: the older watcher is passed over for the fighter.
	CHECK(authority.Leave(a).accepted);
	CHECK(authority.SnapshotView().host == b && FindMember(authority.SnapshotView(), b)->host);
	// The last fighter leaves: the host field stays set, on the watcher, without its powers.
	CHECK(authority.Leave(b).accepted);
	CHECK(authority.SnapshotView().host == w);
	for (const auto kind : {ActionKind::Close, ActionKind::Lock, ActionKind::SetCapacity, ActionKind::Rename}) {
		auto action = RoomAction(authority, w, kind);
		action.capacity = 4; action.locked = true; action.text = "Renamed";
		const auto result = authority.Apply(w, action);
		// Renaming answers Unauthorized for anyone who cannot, the other three NotHost.
		CHECK(!result.accepted && result.reason == (kind == ActionKind::Rename ? RejectReason::Unauthorized : RejectReason::NotHost));
	}
	CHECK(!authority.SnapshotView().closed && !authority.SnapshotView().locked && authority.SnapshotView().name == "Match");
	// A fighter returning takes the host field back, and the room can be run again.
	const MemberId returned = Fighter(authority, 'a');
	CHECK(returned && authority.SnapshotView().host == returned && FindMember(authority.SnapshotView(), returned)->host &&
		!FindMember(authority.SnapshotView(), w)->host);
	auto lock = RoomAction(authority, returned, ActionKind::Lock);
	lock.locked = true;
	CHECK(authority.Apply(returned, lock).accepted);
}

// The binding's wire form: a room without watchers keeps its bytes, watchers
// round trip, and a binding that names too many, one twice or a fighter as a
// watcher is not a binding.
static void TestBindingForms() {
	const auto plain = Binding(1, {});
	CHECK(!nlohmann::json(plain).contains("watchers") && plain.Valid());
	auto watched = Binding(1, {1, 2});
	CHECK(watched.Valid() && nlohmann::json(watched).at("watchers").size() == 2);
	TournamentBinding copy;
	nlohmann::json(watched).get_to(copy);
	CHECK(copy.watchers == watched.watchers && copy.Valid());
	CHECK(copy.WatcherOf(WatcherEndpoint(2)) && !copy.WatcherOf(Endpoint('a')) && copy.SlotOf(WatcherEndpoint(1)) < 0);
	CHECK(copy.EmberIdOf(Endpoint('b')) == "emb1_player2" && copy.EmberIdOf(WatcherEndpoint(1)) == "emb1_watcher1" &&
		copy.EmberIdOf(Endpoint('c')).empty() && TournamentBinding().EmberIdOf(Endpoint('a')).empty() &&
		!TournamentBinding().WatcherOf(WatcherEndpoint(1)));
	// Eight fit; nine do not.
	CHECK(Binding(1, {1, 2, 3, 4, 5, 6, 7, 8}).Valid() && !Binding(1, {1, 2, 3, 4, 5, 6, 7, 8, 9}).Valid());
	auto twice = Binding(1, {1, 1});
	CHECK(!twice.Valid());
	auto sameId = Binding(1, {1, 2});
	sameId.watchers[1].emberId = sameId.watchers[0].emberId;
	CHECK(!sameId.Valid());
	auto fighterEndpoint = Binding(1, {1});
	fighterEndpoint.watchers[0].endpoint = Endpoint('a');
	CHECK(!fighterEndpoint.Valid());
	auto fighterId = Binding(1, {1});
	fighterId.watchers[0].emberId = "emb1_player2";
	CHECK(!fighterId.Valid());
	auto badHex = Binding(1, {1});
	badHex.watchers[0].endpoint = std::string(63, 'a') + "G";
	CHECK(!badHex.Valid());
	// A snapshot that holds one of these is refused when read.
	for (const auto& invalid : {fighterEndpoint, badHex, twice}) {
		TournamentBinding read;
		bool refused = false;
		try { nlohmann::json(invalid).get_to(read); } catch (const std::exception&) { refused = true; }
		CHECK(refused);
	}
	// And a watcher list that is not a list.
	auto malformed = nlohmann::json(watched);
	malformed["watchers"] = "none";
	TournamentBinding read;
	bool refused = false;
	try { malformed.get_to(read); } catch (const std::exception&) { refused = true; }
	CHECK(refused);
}

// The binding and its watchers move with the room to a new owner, who still
// admits them.
static void TestCheckpointKeepsWatchers() {
	Match match({1, 2});
	const MemberId w = Watcher(match.authority, 1);
	CHECK(w);
	RoomAuthority restored("Other", 8, 1);
	CHECK(restored.RestoreCheckpoint(match.authority.Checkpoint()));
	CHECK(restored.SnapshotView().tournament.watchers.size() == 2 && Listed(restored.SnapshotView().tables[0].spectators, w));
	CHECK(IsWatcher(restored.SnapshotView(), w) && WatcherCount(restored.SnapshotView()) == 1);
	CHECK(Watcher(restored, 2));
}

int main() {
	TestBindNamesWatchers();
	TestBindMovesAModifiedWatcher();
	TestJoin();
	TestFighterReturnsToACrowd();
	TestWatchersTakeNoAction();
	TestReadyUnderChurn();
	TestSpectatorListAtStart();
	TestHostStaysWithTheFighters();
	TestBindingForms();
	TestCheckpointKeepsWatchers();
	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("room watch tests passed\n");
	return failures ? 1 : 0;
}
