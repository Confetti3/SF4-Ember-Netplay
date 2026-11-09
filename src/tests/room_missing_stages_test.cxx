// The stages a member's copy cannot load (room::MemberProfile::missingStages,
// SFV Ember's EMBER_LIBRARY.md section 38) through the room: the list as the
// room keeps it, the member's and the join request's wire forms (none at all
// for a member that lacks nothing, so SF4's bytes stay as they were), the
// table queries every stage check asks, P1's Ready taken back at the start,
// and a watcher that sits a game out. SF4's catalog judges the ids: 0, 11
// and 29 are its stages, 22 and -1 are not, and three-character codes are
// its custom stages.
#include "room_authority_support.hxx"
#include "../common/StageCatalog.hxx"
#include "../session/sf4e__SessionProtocol.hxx"

#include <set>
#include <string>
#include <vector>

using Stages = std::vector<std::int64_t>;

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
// MaximumMissingStages; from JSON, a non-array is none and a non-integer
// entry is dropped.
static void TestListKept() {
	EXPECT(MissingStagesFrom(Stages{29, 0, 22, 0, -1, 11}) == (Stages{0, 11, 29}));
	EXPECT(MissingStagesFrom(Stages{}).empty());
	EXPECT(MissingStagesFrom(nlohmann::json("0,11")).empty());
	EXPECT(MissingStagesFrom(nlohmann::json::object()).empty());
	EXPECT(MissingStagesFrom(nlohmann::json(11)).empty());
	EXPECT(MissingStagesFrom(nlohmann::json::parse("[11, \"0\", 1.5, true, null, 29, 11, 22]")) == (Stages{11, 29}));
	Stages many;
	for (int i = 79; i >= 0; --i) {
		const std::string code = "Q" + std::to_string(i / 10) + std::to_string(i % 10);
		const int id = sf4e::selection::CustomStageId(code.c_str());
		EXPECT(id >= 0);
		many.push_back(id);
	}
	const auto kept = MissingStagesFrom(many);
	EXPECT(kept.size() == MaximumMissingStages);
	EXPECT(std::is_sorted(kept.begin(), kept.end()));
	EXPECT(std::set<std::int64_t>(kept.begin(), kept.end()).size() == kept.size());
}

// The member keeps its list through the room's snapshot and checkpoint; a
// member that lacks nothing has no key, as before.
static void TestMemberWire() {
	RoomAuthority authority("Stages", 16, 72);
	const auto lacking = JoinLacking(authority, 0, {29, 11, 22}, true);
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
	member["missing_stages"] = nlohmann::json::array({11, 22, 11});
	EXPECT(member.get<Member>().missingStages == (Stages{11}));
	member.erase("missing_stages");
	EXPECT(member.get<Member>().missingStages.empty());
}

// The join request carries the list only when it is not empty: an SF4
// player's request has exactly the keys it had.
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
	for (const nlohmann::json invalid : {nlohmann::json("11"), nlohmann::json::object(), nlohmann::json::array({22, -1}),
			 nlohmann::json(true)}) {
		auto forged = sent;
		forged["missingStages"] = invalid;
		EXPECT(forged.get<protocol::SessionJoinRequest>().Profile().missingStages.empty());
	}
	auto repeated = sent;
	repeated["missingStages"] = nlohmann::json::array({29, "x", 29, 0});
	EXPECT(repeated.get<protocol::SessionJoinRequest>().Profile().missingStages == (Stages{0, 29}));
}

// The table's stages are both seats' lists; a player's unavailable stages
// are those when seated, their own copy's anywhere else.
static void TestTableQueries() {
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
	EXPECT(UnavailableStagesFor(asWatcher, {0}) == (Stages{0}));
	EXPECT(UnavailableStagesFor(Snapshot{}, {5, 6}) == (Stages{5, 6}));
	// An SF4 room: nobody lacks anything, so every stage is playable.
	RoomAuthority sf4("SF4", 16, 1);
	const auto a = Join(sf4, 0, true), b = Join(sf4, 1);
	for (const auto member : {a, b}) EXPECT(sf4.Apply(member, TableAction(sf4, member, 0, ActionKind::Queue)).accepted);
	EXPECT(TableMissingStages(sf4.SnapshotView(), sf4.SnapshotView().tables[0]).empty());
	EXPECT(StagePlayableAt(sf4.SnapshotView(), sf4.SnapshotView().tables[0], 11));
	EXPECT(StagePlayableAt(sf4.SnapshotView(), sf4.SnapshotView().tables[0], -1));
}

// At the start, P1's Ready is taken back for a stage the opponent lacks, and
// the table hears StageNotShared with no generation; a game on a stage both
// fighters have begins, and a watcher whose copy lacks it is left out of the
// game's roster and hears StageNotShared with the game's generation.
static void TestStartRefusalAndWatchers() {
	RoomAuthority authority("Start", 16, 1);
	const auto p1 = JoinLacking(authority, 0, {}, true), p2 = JoinLacking(authority, 1, {29});
	const auto lackingWatcher = JoinLacking(authority, 2, {11}), watcher = Join(authority, 3);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	for (const auto member : {lackingWatcher, watcher})
		EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Watch)).accepted);
	EXPECT(!authority.WithdrawStage(0).accepted); // nobody is ready
	EXPECT(authority.Apply(p2, TableAction(authority, p2, 0, ActionKind::Ready)).accepted);
	EXPECT(!authority.WithdrawStage(0).accepted); // the game is not about to start
	EXPECT(Has(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).events, Event::Kind::MatchReady));
	const auto withdrawn = authority.WithdrawStage(0);
	EXPECT(withdrawn.accepted);
	EXPECT(Has(withdrawn.events, Event::Kind::StageNotShared, p1, 0));
	EXPECT(!Has(withdrawn.events, Event::Kind::MatchReady) && !Has(withdrawn.events, Event::Kind::MatchStarted));
	const auto& table = authority.SnapshotView().tables[0];
	EXPECT(!table.ready[0] && table.ready[1] && table.phase == TablePhase::Waiting && table.matchGeneration == 0);
	// The wire takes the new kind.
	const auto event = nlohmann::json(withdrawn.events.back()).get<Event>();
	EXPECT(event.kind == Event::Kind::StageNotShared && event.member == p1);

	// P1 readies again; the game begins on stage 11, which the fighters have.
	EXPECT(Has(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).events, Event::Kind::MatchReady));
	const auto begun = authority.BeginMatch(0, p1, p2, 11);
	EXPECT(begun.accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	EXPECT(generation != 0 && Has(begun.events, Event::Kind::MatchStarted));
	EXPECT(Has(begun.events, Event::Kind::StageNotShared, lackingWatcher, generation));
	EXPECT(!Has(begun.events, Event::Kind::StageNotShared, watcher, generation));
	const auto roster = authority.MatchRoster(0);
	EXPECT(std::find(roster.begin(), roster.end(), watcher) != roster.end());
	EXPECT(std::find(roster.begin(), roster.end(), lackingWatcher) == roster.end());
	EXPECT(std::find(roster.begin(), roster.end(), p1) != roster.end() && std::find(roster.begin(), roster.end(), p2) != roster.end());
	// It keeps its place for the next game.
	const auto& spectators = authority.SnapshotView().tables[0].spectators;
	EXPECT(std::find(spectators.begin(), spectators.end(), lackingWatcher) != spectators.end());
}

// A start without a stage known (-1, as every caller but the session owner)
// leaves every watcher in.
static void TestStartWithoutStage() {
	RoomAuthority authority("No stage", 16, 1);
	const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1);
	const auto lackingWatcher = JoinLacking(authority, 2, {11});
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.Apply(lackingWatcher, TableAction(authority, lackingWatcher, 0, ActionKind::Watch)).accepted);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	const auto begun = authority.BeginMatch(0, p1, p2);
	EXPECT(begun.accepted && !Has(begun.events, Event::Kind::StageNotShared));
	const auto roster = authority.MatchRoster(0);
	EXPECT(std::find(roster.begin(), roster.end(), lackingWatcher) != roster.end());
}

int main() {
	TestListKept();
	TestMemberWire();
	TestJoinRequest();
	TestTableQueries();
	TestStartRefusalAndWatchers();
	TestStartWithoutStage();
	if (TEST_FAILURES()) return 1;
	std::puts("RoomMissingStages test passed");
	return 0;
}
