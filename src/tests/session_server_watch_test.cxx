// The session server leading a room bound to a tournament match that names
// watchers: they join as observers and are placed as spectators, a bind that
// drops one sends it away, and what the server commits for the helper to read
// is the shared fixture (tests/fixtures/tournament_watch_checkpoint.json).
#include "../session/sf4e__SessionServer.hxx"

#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <zlib.h>

#include "server_transport_support.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

#ifndef SF4E_TEST_FIXTURES
#error "SF4E_TEST_FIXTURES names the fixtures directory"
#endif

static std::string Endpoint(session::Connection connection) {
	// Distinct for the sixteen connections a room holds; 1 to 4 are a, b, c and e (a run of d is
	// not a key the helper's endpoint parser takes, the fixture being parsed there too).
	return std::string(64, "0abcedf123456789"[static_cast<std::size_t>(connection) % 16]);
}

// A checkpoint append as the helper compresses it (coordination.rs, Compression::fast()).
static std::size_t FastZlibSize(const std::string& text) {
	uLongf size = compressBound(static_cast<uLong>(text.size()));
	std::vector<Bytef> out(size);
	CHECK(compress2(out.data(), &size, reinterpret_cast<const Bytef*>(text.data()), static_cast<uLong>(text.size()), Z_BEST_SPEED) == Z_OK);
	return size;
}

// A writable server leading a room whose two fighters are connections 1 and 2;
// watchers join as connections 3 and up. Every change is committed.
struct WatchRoom {
	MockTransport* transport = new MockTransport();
	SessionServer server{"tournament", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	const std::uint64_t term = 3;
	std::uint64_t request = 1, revision = 0, actionId = 0;
	std::shared_ptr<const session::SessionProposal> committed;
	// What the leader has appended so far: the commits, and the bytes of each
	// proposal as it is and as the helper sends it (zlib, fastest level).
	std::uint64_t commits = 0;
	std::size_t rawBytes = 0, wireBytes = 0;

	WatchRoom() {
		std::array<std::uint8_t, 16> roomId = {};
		roomId[0] = 79;
		server.EnableMatchAuthorization(roomId, [](session::Connection connection) { return Endpoint(connection); });
		server.EnableCustomRooms("Match", 16, 51);
		server.SetAuthority(term, revision, true);
		Admit(1, "Player 1", false);
		Admit(2, "Player 2", false);
	}
	void Commit() {
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		committed = server.PendingProposal();
		const auto text = json(*committed).dump();
		++commits;
		rawBytes += text.size();
		wireBytes += FastZlibSize(text);
		CHECK(server.ApplyCommit(request, term, revision + 1, committed->checkpoint, committed->effectsDigest));
		++request; ++revision;
	}
	void Admit(session::Connection connection, const std::string& name, bool observer) {
		protocol::SessionJoinRequest join;
		join.username = name; join.sidecarHash = "build"; join.port = 30000;
		join.customRooms = true; join.roomProtocol = room::ProtocolVersion; join.observer = observer;
		protocol::SessionHelloMsg hello; hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		if (server.HasRecoveryCandidate()) Commit();
	}
	static room::TournamentBinding Binding(std::uint64_t revision, std::size_t watchers) {
		room::TournamentBinding binding;
		binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
		binding.assignmentGeneration = 1;
		binding.bindingRevision = revision;
		binding.gamesToWin = 3;
		binding.fighters[0] = {Endpoint(1), "emb1_player1"};
		binding.fighters[1] = {Endpoint(2), "emb1_player2"};
		for (std::size_t i = 0; i < watchers; ++i)
			binding.watchers.push_back({Endpoint(static_cast<session::Connection>(3 + i)), "emb1_watcher" + std::to_string(i + 1)});
		return binding;
	}
	void Bind(std::uint64_t revision, std::size_t watchers) {
		CHECK(server.BindTournament(Binding(revision, watchers)));
		Commit();
	}
	// Binds the room to the match naming `watchers` of them, and has each join.
	void BindAndWatch(std::size_t watchers) {
		Bind(1, watchers);
		for (std::size_t i = 0; i < watchers; ++i) Admit(static_cast<session::Connection>(3 + i), "Watcher " + std::to_string(i + 1), true);
	}
	const room::Snapshot& Snapshot() const { return *server.RoomSnapshot(); }
	const room::Table& Table() const { return server._roomAuthority->SnapshotView().tables[room::TournamentTable]; }
	// A room action from a member's connection, committed when it changes the room.
	void Act(session::Connection by, room::ActionKind kind, std::uint64_t generation, room::MatchResult result = room::MatchResult::Abort) {
		const auto& snapshot = server._roomAuthority->SnapshotView();
		room::Action action;
		action.kind = kind; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
		action.table = room::TournamentTable; action.tableRevision = snapshot.tables[room::TournamentTable].revision;
		action.actionId = ++actionId;
		action.matchGeneration = generation;
		action.text = "per_one";
		action.startWindowMs = room::PermitStartMs;
		action.result = result;
		protocol::RoomActionMessage message; message.action = action;
		transport->Push(by, json(message));
		CHECK(server.Step() == 0);
		if (server.HasRecoveryCandidate()) Commit();
	}
	// Every member's connection says `type` of the game being set up; whatever it leads to is committed.
	void Acknowledge(const char* type, std::uint64_t generation, std::size_t members) {
		for (session::Connection connection = 1; connection <= members; ++connection) {
			transport->Push(connection, json{{"type", type}, {"generation", generation}});
			CHECK(server.Step() == 0);
			if (server.HasRecoveryCandidate()) Commit();
		}
	}
	// One game, from the fighters' Ready to every recipient's acknowledgment of
	// its end: the room's whole traffic between two games. The members are the
	// connections 1 to `members`, the fighters first.
	void PlayOneGame(std::size_t members) {
		server.AdvanceCustomRoom(1000);
		Act(1, room::ActionKind::Ready, 0);
		Act(2, room::ActionKind::Ready, 0);
		const auto generation = Table().permitGeneration;
		CHECK(generation != 0);
		Act(1, room::ActionKind::PermitReady, generation);
		Act(2, room::ActionKind::PermitReady, generation);
		Acknowledge("game_prepared", generation, members);
		Acknowledge("game_ready", generation, members);
		CHECK(Table().phase == room::TablePhase::Playing && Table().matchGeneration == generation);
		Act(1, room::ActionKind::RecordResult, generation, room::MatchResult::P1Win);
		Act(2, room::ActionKind::RecordResult, generation, room::MatchResult::P1Win);
		CHECK(Table().phase != room::TablePhase::Playing);
		for (session::Connection connection = 1; connection <= members; ++connection)
			Act(connection, room::ActionKind::AcknowledgeTerminal, generation);
	}
	std::size_t SentTo(session::Connection connection, const char* type) const {
		std::size_t count = 0;
		for (const auto& sent : transport->outgoing)
			if (sent.first == connection && sent.second.value("type", std::string()) == type) ++count;
		return count;
	}
};

static std::string Read(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	CHECK(file.good());
	std::ostringstream text;
	text << file.rdbuf();
	return text.str();
}

// What the helper reads of a committed proposal: each member's endpoint and the
// room snapshot's tournament object, at the paths it reads them from. The rest
// of the proposal is the game's own and stays out of the contract.
static json WhatTheHelperReads(const json& proposal) {
	const auto& checkpoint = proposal.at("checkpoint");
	json members = json::array();
	for (const auto& member : checkpoint.at("members"))
		members.push_back({{"data", {{"authenticatedEndpoint", member.at("data").at("authenticatedEndpoint")}}}});
	const auto& room = checkpoint.at("room");
	return {{"checkpoint", {{"members", members},
		{"room", {{"version", room.at("version")}, {"snapshot", {{"tournament", room.at("snapshot").at("tournament")}}}}}}}};
}

// The proposal a server commits for a bound room with two watchers is the
// shared fixture, which the helper's own test parses: a change to either side
// shows in both. SF4E_WRITE_FIXTURE=1 writes the fixture again.
static void TestFixtureIsWhatTheServerCommits() {
	WatchRoom room;
	room.BindAndWatch(2);
	CHECK(room.Snapshot().members.size() == 4 && room.Snapshot().tournament.watchers.size() == 2);
	const auto produced = WhatTheHelperReads(json(*room.committed));
	const std::string path = std::string(SF4E_TEST_FIXTURES) + "/tournament_watch_checkpoint.json";
	const char* write = std::getenv("SF4E_WRITE_FIXTURE");
	if (write && std::string(write) == "1") {
		std::ofstream(path, std::ios::binary) << produced.dump(2) << "\n";
		std::printf("fixture written to %s\n", path.c_str());
	}
	CHECK(json::parse(Read(path)) == produced);
	// The watchers are named in the tournament object, oldest first, and their endpoints are members'.
	const auto& tournament = produced.at("checkpoint").at("room").at("snapshot").at("tournament");
	CHECK(tournament.at("watchers").size() == 2);
	CHECK(tournament.at("watchers").at(0).at("endpoint") == Endpoint(3) && tournament.at("watchers").at(0).at("ember_id") == "emb1_watcher1");
	CHECK(tournament.at("fighters").at(1).at("endpoint") == Endpoint(2));
	CHECK(produced.at("checkpoint").at("members").size() == 4);
}

// Watchers join as observers and are spectators at once, through the session
// server, without disturbing the fighters' seats or table 0's revision.
static void TestWatchersJoinAsObservers() {
	WatchRoom room;
	room.Bind(1, 2);
	const auto tableRevision = room.Snapshot().tables[room::TournamentTable].revision;
	const auto roomRevision = room.Snapshot().revision;
	room.Admit(3, "Watcher 1", true);
	room.Admit(4, "Watcher 2", true);
	const auto& snapshot = room.Snapshot();
	const auto& table = snapshot.tables[room::TournamentTable];
	CHECK(snapshot.members.size() == 4 && table.spectators.size() == 2);
	CHECK(table.p1 && table.p2 && table.revision == tableRevision && snapshot.revision > roomRevision);
	// An observer the binding does not name, and one that omits the flag but is a stranger, are refused.
	room.Admit(5, "Watcher 3", true);
	room.Admit(6, "Stranger", false);
	CHECK(room.Snapshot().members.size() == 4);
}

// The flag travels in the join request only when set, and an unset one reads as false.
static void TestObserverFlagOnTheWire() {
	protocol::SessionJoinRequest plain;
	CHECK(!json(plain).contains("observer") && !plain.Profile().observer);
	protocol::SessionJoinRequest watching;
	watching.observer = true;
	CHECK(json(watching).at("observer") == true);
	CHECK(json(watching).get<protocol::SessionJoinRequest>().observer && json(watching).get<protocol::SessionJoinRequest>().Profile().observer);
	auto tampered = json(watching);
	tampered["observer"] = "yes";
	CHECK(!tampered.get<protocol::SessionJoinRequest>().observer);
	room::MemberProfile profile;
	profile.observer = true;
	protocol::SessionJoinRequest copied;
	copied.SetProfile(profile);
	CHECK(copied.observer);
}

// A bind that no longer names a watcher sends it away as kicked, and tells
// everyone, while the other watcher and the fighters stay.
static void TestRevokedWatcherIsSentAway() {
	WatchRoom room;
	room.BindAndWatch(2);
	room.transport->outgoing.clear();
	room.Bind(2, 1);
	CHECK(room.Snapshot().members.size() == 3 && room.Snapshot().tournament.watchers.size() == 1);
	bool kicked = false;
	for (const auto& sent : room.transport->outgoing)
		if (sent.first == 4 && sent.second.value("type", std::string()) == "room_result" &&
			sent.second.at("result").value("reason", 0) == static_cast<int>(room::RejectReason::MemberKicked)) kicked = true;
	CHECK(kicked);
	CHECK(room.SentTo(1, "room_snapshot") > 0);
}

// What the leader commits grows with each watcher: every append carries the
// whole checkpoint, and every watcher is a learner of it. This measures, for 0
// to 8 watchers, the commits and bytes that open the room, and the commits and
// bytes of one game from Ready to the last acknowledgment of its end. The bytes
// are the helper's wire form of each append (zlib, fastest level), so the
// leader's upload is that times the learners (the other fighter and each
// watcher). MATCH_WATCH.md section 6.6 records the numbers. The limits are the
// measured values with a margin: a change that makes the traffic or the append
// larger fails here, to be looked at, not raised without a reason.
struct Cost {
	std::size_t watchers = 0;
	// Opening the room: the last append raw and on the wire, the commits, and all their wire bytes.
	std::size_t openRaw = 0, openWire = 0, openTotalWire = 0;
	std::uint64_t openCommits = 0;
	// One game on top of it.
	std::uint64_t gameCommits = 0;
	std::size_t gameWire = 0;
};
// Measured (section 6.6). The counts are the measured ones: they are structural
// (one commit to admit each watcher, three more per watcher for each game), so
// one more is a change to look at. The bytes are the measured ones plus a quarter,
// rounded up.
// watchers, open: last append, on the wire, all on the wire, commits; game: commits, on the wire
static const Cost Limits[] = {
	{0, 39000, 4000, 9500, 3, 12, 110500},
	{2, 96500, 7500, 22500, 5, 18, 327000},
	{4, 204500, 13500, 46000, 7, 24, 555000},
	{6, 378000, 22500, 86500, 9, 30, 717500},
	{8, 476500, 26500, 140000, 11, 36, 887000},
};

static Cost Measure(std::size_t watchers) {
	WatchRoom room;
	room.BindAndWatch(watchers);
	CHECK(room.Snapshot().members.size() == 2 + watchers);
	Cost cost;
	cost.watchers = watchers;
	const auto text = json(*room.committed).dump();
	cost.openRaw = text.size();
	cost.openWire = FastZlibSize(text);
	cost.openCommits = room.commits;
	cost.openTotalWire = room.wireBytes;
	const auto commitsBefore = room.commits;
	const auto wireBefore = room.wireBytes;
	room.PlayOneGame(2 + watchers);
	cost.gameCommits = room.commits - commitsBefore;
	cost.gameWire = room.wireBytes - wireBefore;
	return cost;
}

static void TestTrafficByWatchers() {
	std::vector<Cost> costs;
	for (const auto& limit : Limits) {
		costs.push_back(Measure(limit.watchers));
		const auto& cost = costs.back();
		std::printf("watch measurement: %zu watchers: room open %llu commits, last append %zu bytes (%zu on the wire), %zu on the wire in all; "
			"one game %llu commits, %zu bytes on the wire\n",
			cost.watchers, static_cast<unsigned long long>(cost.openCommits), cost.openRaw, cost.openWire, cost.openTotalWire,
			static_cast<unsigned long long>(cost.gameCommits), cost.gameWire);
	}
	for (std::size_t i = 0; i < costs.size(); ++i) {
		const auto& cost = costs[i];
		const auto& limit = Limits[i];
		CHECK(cost.openRaw <= limit.openRaw && cost.openWire <= limit.openWire && cost.openTotalWire <= limit.openTotalWire);
		CHECK(cost.openCommits <= limit.openCommits && cost.gameCommits <= limit.gameCommits && cost.gameWire <= limit.gameWire);
		// More watchers never cost less.
		if (i) CHECK(cost.openRaw >= costs[i - 1].openRaw && cost.openCommits >= costs[i - 1].openCommits && cost.gameWire >= costs[i - 1].gameWire);
	}
}

int main() {
	TestFixtureIsWhatTheServerCommits();
	TestWatchersJoinAsObservers();
	TestObserverFlagOnTheWire();
	TestRevokedWatcherIsSentAway();
	TestTrafficByWatchers();
	std::printf("session server watch tests passed\n");
	return 0;
}
