// The stages a member's copy cannot load (room::MemberProfile::missingStages,
// SFV Ember's EMBER_LIBRARY.md section 38) through the room: the list as the
// session owner reads it, the member's and the join request's wire forms
// (none at all for a member that lacks nothing, so SF4's bytes stay as they
// were), the table queries every stage check asks, the start that refuses a
// stage (P1's Ready taken back), and a watcher that sits a game out.
// It defines Catalog() from the fake game's (fake_catalog.hxx) rather than
// linking sf4e_catalog (cmake/sf4e_test_targets.cmake): a game whose copies
// can lack stages, with stages 0 to 999 (1000 and -1 are not stages), and
// the same game under SF4's rule, whose copies cannot.
#include "fake_catalog.hxx"
#include "room_authority_support.hxx"
#include "../session/sf4e__SessionProtocol.hxx"

#include <set>
#include <string>
#include <vector>

using Stages = std::vector<std::int64_t>;

namespace {
bool WideStage(std::int64_t id) { return id >= 0 && id < 1000; }
ember::GameCatalog Catalogue(bool stagesCanBeMissing) {
	ember::GameCatalog catalog = ember::test::TestCatalog(ember::test::CatalogFighters);
	catalog.validStage = &WideStage;
	catalog.stagesCanBeMissing = stagesCanBeMissing;
	return catalog;
}
ember::GameCatalog current = Catalogue(true);
}

namespace ember {
const GameCatalog& Catalog() { return current; }
}

static MemberId JoinLacking(RoomAuthority& authority, int index, Stages missing, bool host = false) {
	MemberProfile profile;
	profile.missingStages = std::move(missing);
	const auto result = authority.Join("Player" + std::to_string(index), Peer(index), host, profile);
	EXPECT(result.accepted);
	return result.accepted ? result.snapshot.members.back().id : 0;
}

static bool Has(const std::vector<Event>& events, Event::Kind kind, MemberId member, std::uint64_t generation) {
	return std::any_of(events.begin(), events.end(), [&](const Event& event) {
		return event.kind == kind && event.member == member && event.matchGeneration == generation;
	});
}

static bool Has(const std::vector<Event>& events, Event::Kind kind) {
	return std::any_of(events.begin(), events.end(), [&](const Event& event) { return event.kind == kind; });
}

// Valid stages of the catalog, ascending, without repeats, at most
// MaximumMissingStages (the lowest ids); from JSON, a non-array is none and a
// non-integer entry is dropped.
static void TestListKept() {
	EXPECT(MissingStagesFrom(Stages{29, 0, 1000, 0, -1, 11}) == (Stages{0, 11, 29}));
	EXPECT(MissingStagesFrom(Stages{}).empty());
	EXPECT(MissingStagesFrom(nlohmann::json("0,11")).empty());
	EXPECT(MissingStagesFrom(nlohmann::json::object()).empty());
	EXPECT(MissingStagesFrom(nlohmann::json(11)).empty());
	EXPECT(MissingStagesFrom(nlohmann::json::parse("[11, \"0\", 1.5, true, null, 29, 11, 1000]")) == (Stages{11, 29}));
	Stages many;
	for (int id = 79; id >= 0; --id) many.push_back(id);
	const auto kept = MissingStagesFrom(many);
	EXPECT(kept.size() == MaximumMissingStages);
	EXPECT(std::is_sorted(kept.begin(), kept.end()));
	EXPECT(std::set<std::int64_t>(kept.begin(), kept.end()).size() == kept.size());
	EXPECT(kept.front() == 0 && kept.back() == static_cast<std::int64_t>(MaximumMissingStages) - 1);
}

// Under SF4's rule (GameCatalog::stagesCanBeMissing off) every list reads as
// none: from a join request, a member's JSON and the list itself. A forged
// join cannot bring a list into an SF4 room.
static void TestNoneUnderSf4Rule() {
	current = Catalogue(false);
	EXPECT(MissingStagesFrom(Stages{0, 11, 29}).empty());
	EXPECT(MissingStagesFrom(nlohmann::json::array({0, 11, 29})).empty());
	sf4e::SessionProtocol::SessionJoinRequest forged;
	forged.username = "Player"; forged.sidecarHash = "build"; forged.port = 30000;
	nlohmann::json join = forged;
	join["missingStages"] = nlohmann::json::array({11, 29});
	EXPECT(join.get<sf4e::SessionProtocol::SessionJoinRequest>().Profile().missingStages.empty());
	RoomAuthority authority("SF4 rule", 16, 72);
	const auto member = Join(authority, 0, true);
	auto written = nlohmann::json(*FindMember(authority.SnapshotView(), member));
	EXPECT(!written.contains("missing_stages"));
	written["missing_stages"] = nlohmann::json::array({11, 29});
	EXPECT(written.get<Member>().missingStages.empty());
	current = Catalogue(true);
}

// The member keeps its list, as the join's reader kept it, through the room's
// snapshot and checkpoint; a member that lacks nothing has no key, as before.
static void TestMemberWire() {
	RoomAuthority authority("Stages", 16, 72);
	const auto lacking = JoinLacking(authority, 0, {11, 29}, true);
	const auto full = Join(authority, 1);
	const auto& snapshot = authority.SnapshotView();
	const auto* first = FindMember(snapshot, lacking);
	const auto* second = FindMember(snapshot, full);
	EXPECT(first && first->missingStages == (Stages{11, 29}));
	EXPECT(second && second->missingStages.empty());
	const auto lackingJson = nlohmann::json(*first), fullJson = nlohmann::json(*second);
	EXPECT(lackingJson.at("missing_stages") == nlohmann::json::array({11, 29}));
	EXPECT(!fullJson.contains("missing_stages"));
	const auto read = nlohmann::json::parse(nlohmann::json(snapshot).dump()).get<Snapshot>();
	EXPECT(FindMember(read, lacking)->missingStages == (Stages{11, 29}));
	EXPECT(FindMember(read, full)->missingStages.empty());
	RoomAuthority restored("Restore");
	EXPECT(restored.RestoreCheckpoint(authority.Checkpoint()));
	EXPECT(FindMember(restored.SnapshotView(), lacking)->missingStages == (Stages{11, 29}));
	EXPECT(FindMember(restored.SnapshotView(), full)->missingStages.empty());
	// A list that makes no sense reads as none, never as a refused member.
	auto member = lackingJson;
	member["missing_stages"] = "11";
	EXPECT(member.get<Member>().missingStages.empty());
	member["missing_stages"] = nlohmann::json::array({11, 1000, 11});
	EXPECT(member.get<Member>().missingStages == (Stages{11}));
	member.erase("missing_stages");
	EXPECT(member.get<Member>().missingStages.empty());
}

// The join request carries the list only when it is not empty: an SF4
// player's request has exactly the keys it had. Its reader is where the
// session owner checks a joiner's list, the one check.
static void TestJoinRequest() {
	namespace protocol = sf4e::SessionProtocol;
	protocol::SessionJoinRequest plain;
	plain.username = "Player"; plain.sidecarHash = "build"; plain.port = 30000;
	plain.SetProfile(MemberProfile{});
	const nlohmann::json sf4 = plain;
	const std::set<std::string> before = {"type", "sidecarHash", "username", "port", "customRooms", "roomProtocol",
		"mainFighter", "link", "nat", "roomChatDelta"};
	std::set<std::string> keys;
	for (auto item = sf4.begin(); item != sf4.end(); ++item) keys.insert(item.key());
	EXPECT(keys == before);
	EXPECT(sf4.get<protocol::SessionJoinRequest>().Profile().missingStages.empty());

	protocol::SessionJoinRequest lacking = plain;
	MemberProfile profile;
	profile.missingStages = {11, 29};
	lacking.SetProfile(profile);
	const nlohmann::json sent = lacking;
	EXPECT(sent.at("missingStages") == nlohmann::json::array({11, 29}));
	EXPECT(sent.get<protocol::SessionJoinRequest>().Profile().missingStages == (Stages{11, 29}));
	// A forged or malformed list joins as a member that lacks nothing it names.
	for (const nlohmann::json invalid : {nlohmann::json("11"), nlohmann::json::object(), nlohmann::json::array({1000, -1}),
			 nlohmann::json(true)}) {
		auto forged = sent;
		forged["missingStages"] = invalid;
		EXPECT(forged.get<protocol::SessionJoinRequest>().Profile().missingStages.empty());
	}
	auto repeated = sent;
	repeated["missingStages"] = nlohmann::json::array({29, "x", 29, 0});
	EXPECT(repeated.get<protocol::SessionJoinRequest>().Profile().missingStages == (Stages{0, 29}));
}

// The table's stages are both seats' lists; a player's unavailable stages are
// their own copy's and, seated, the table's too, own taken as the game gives
// it rather than as the room holds it.
static void TestTableQueries() {
	EXPECT(StageUnion({1, 3}, {2, 3}) == (Stages{1, 2, 3}));
	EXPECT(StageUnion({}, {4}) == (Stages{4}) && StageUnion({4}, {}) == (Stages{4}) && StageUnion({}, {}).empty());
	RoomAuthority authority("Queries", 16, 1);
	const auto p1 = JoinLacking(authority, 0, {11}, true), p2 = JoinLacking(authority, 1, {29}), watcher = JoinLacking(authority, 2, {0});
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.Apply(watcher, TableAction(authority, watcher, 0, ActionKind::Watch)).accepted);
	const auto& room = authority.SnapshotView();
	const auto& table = room.tables[0];
	EXPECT(TableMissingStages(room, table) == (Stages{11, 29}));
	EXPECT(!StagePlayableAt(room, table, 11) && !StagePlayableAt(room, table, 29));
	EXPECT(StagePlayableAt(room, table, 0)); // only the watcher lacks it
	EXPECT(TableMissingStages(room, room.tables[1]).empty());
	Snapshot asP1 = room, asWatcher = room;
	asP1.localMember = p1;
	asWatcher.localMember = watcher;
	EXPECT(UnavailableStagesFor(asP1, {11}) == (Stages{11, 29}));
	// A stage this copy lacks that the room's copy of its list does not hold
	// (a list the session owner cut short) is still unavailable to it.
	EXPECT(UnavailableStagesFor(asP1, {5, 11}) == (Stages{5, 11, 29}));
	EXPECT(UnavailableStagesFor(asWatcher, {0}) == (Stages{0}));
	EXPECT(UnavailableStagesFor(Snapshot{}, {5, 6}) == (Stages{5, 6}));
	// A room of copies that lack nothing, as every SF4 room: every stage is playable.
	RoomAuthority sf4("SF4", 16, 1);
	const auto a = Join(sf4, 0, true), b = Join(sf4, 1);
	for (const auto member : {a, b}) EXPECT(sf4.Apply(member, TableAction(sf4, member, 0, ActionKind::Queue)).accepted);
	EXPECT(TableMissingStages(sf4.SnapshotView(), sf4.SnapshotView().tables[0]).empty());
	EXPECT(StagePlayableAt(sf4.SnapshotView(), sf4.SnapshotView().tables[0], 11));
	Snapshot asA = sf4.SnapshotView();
	asA.localMember = a;
	EXPECT(UnavailableStagesFor(asA, {}).empty());
}

// BeginMatch judges a sent stage. One the opponent lacks, or one that is not
// the game's, starts no game: P1's Ready is taken back and the table hears
// StageNotShared, which the wire reads. A game on a stage both fighters have
// begins, and a watcher whose copy lacks it is left out of the game's roster
// and hears WatcherLacksStage with the game's generation.
static void TestStartRefusalAndWatchers() {
	RoomAuthority authority("Start", 16, 1);
	const auto p1 = JoinLacking(authority, 0, {}, true), p2 = JoinLacking(authority, 1, {29});
	const auto lackingWatcher = JoinLacking(authority, 2, {11}), watcher = Join(authority, 3);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	for (const auto member : {lackingWatcher, watcher})
		EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Watch)).accepted);
	// Nothing is withdrawn from a table whose game is not about to start.
	EXPECT(authority.Apply(p2, TableAction(authority, p2, 0, ActionKind::Ready)).accepted);
	EXPECT(!authority.BeginMatch(0, p1, p2, MatchStage::Sent(29)).accepted);
	EXPECT(authority.SnapshotView().tables[0].ready[1]);

	for (const std::int64_t refused : {std::int64_t(29), std::int64_t(1000)}) {
		EXPECT(Has(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).events, Event::Kind::MatchReady));
		const auto withdrawn = authority.BeginMatch(0, p1, p2, MatchStage::Sent(refused));
		EXPECT(withdrawn.accepted);
		EXPECT(Has(withdrawn.events, Event::Kind::StageNotShared, p1, 0));
		EXPECT(!Has(withdrawn.events, Event::Kind::MatchReady) && !Has(withdrawn.events, Event::Kind::MatchStarted));
		const auto& table = authority.SnapshotView().tables[0];
		EXPECT(!table.ready[0] && table.ready[1] && table.phase == TablePhase::Waiting && table.matchGeneration == 0);
		const auto event = nlohmann::json(withdrawn.events.back()).get<Event>();
		EXPECT(event.kind == Event::Kind::StageNotShared && event.member == p1);
	}

	// P1 readies again; the game begins on stage 11, which the fighters have.
	EXPECT(Has(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).events, Event::Kind::MatchReady));
	const auto begun = authority.BeginMatch(0, p1, p2, MatchStage::Sent(11));
	EXPECT(begun.accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	EXPECT(generation != 0 && Has(begun.events, Event::Kind::MatchStarted));
	EXPECT(!Has(begun.events, Event::Kind::StageNotShared));
	EXPECT(Has(begun.events, Event::Kind::WatcherLacksStage, lackingWatcher, generation));
	EXPECT(!Has(begun.events, Event::Kind::WatcherLacksStage, watcher, generation));
	const auto sitting = std::find_if(begun.events.begin(), begun.events.end(),
		[](const Event& event) { return event.kind == Event::Kind::WatcherLacksStage; });
	EXPECT(sitting != begun.events.end() && nlohmann::json(*sitting).get<Event>().kind == Event::Kind::WatcherLacksStage);
	const auto roster = authority.MatchRoster(0);
	EXPECT(std::find(roster.begin(), roster.end(), watcher) != roster.end());
	EXPECT(std::find(roster.begin(), roster.end(), lackingWatcher) == roster.end());
	EXPECT(std::find(roster.begin(), roster.end(), p1) != roster.end() && std::find(roster.begin(), roster.end(), p2) != roster.end());
	// It keeps its place for the next game.
	const auto& spectators = authority.SnapshotView().tables[0].spectators;
	EXPECT(std::find(spectators.begin(), spectators.end(), lackingWatcher) != spectators.end());
}

// A game P1 sent no stage for starts as one always has: with every watcher,
// and no event about stages.
static void TestStartWithoutStage() {
	RoomAuthority authority("No stage", 16, 1);
	const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1);
	const auto lackingWatcher = JoinLacking(authority, 2, {11});
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.Apply(lackingWatcher, TableAction(authority, lackingWatcher, 0, ActionKind::Watch)).accepted);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	const auto begun = authority.BeginMatch(0, p1, p2, MatchStage::Unsent());
	EXPECT(begun.accepted && Has(begun.events, Event::Kind::MatchStarted));
	EXPECT(!Has(begun.events, Event::Kind::StageNotShared) && !Has(begun.events, Event::Kind::WatcherLacksStage));
	const auto roster = authority.MatchRoster(0);
	EXPECT(std::find(roster.begin(), roster.end(), lackingWatcher) != roster.end());
}

int main() {
	TestListKept();
	TestNoneUnderSf4Rule();
	TestMemberWire();
	TestJoinRequest();
	TestTableQueries();
	TestStartRefusalAndWatchers();
	TestStartWithoutStage();
	if (TEST_FAILURES()) return 1;
	std::puts("RoomMissingStages test passed");
	return 0;
}
