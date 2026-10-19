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

// A writable server leading a room whose two fighters are connections 1 and 2;
// watchers join as connections 3 and up. Every change is committed.
struct WatchRoom {
	MockTransport* transport = new MockTransport();
	SessionServer server{"tournament", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	const std::uint64_t term = 3;
	std::uint64_t request = 1, revision = 0;
	std::shared_ptr<const session::SessionProposal> committed;

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

// What the leader commits grows with each watcher: the raw proposal at 0, 4
// and 8 watchers, and the commits it took to open the room. The helper
// compresses it (zlib, coordination.rs), so its own test measures the wire
// bytes; this is the input the helper replicates to each watcher.
static void TestProposalSizeByWatchers() {
	std::size_t sizes[3] = {};
	const std::size_t counts[3] = {0, 4, 8};
	for (int i = 0; i < 3; ++i) {
		WatchRoom room;
		room.BindAndWatch(counts[i]);
		const auto before = room.revision;
		sizes[i] = json(*room.committed).dump().size();
		std::printf("watch measurement: %zu watchers, proposal %zu bytes, %llu commits to open the room\n",
			counts[i], sizes[i], static_cast<unsigned long long>(before));
	}
	CHECK(sizes[0] < sizes[1] && sizes[1] < sizes[2]);
}

int main() {
	TestFixtureIsWhatTheServerCommits();
	TestWatchersJoinAsObservers();
	TestObserverFlagOnTheWire();
	TestRevokedWatcherIsSentAway();
	TestProposalSizeByWatchers();
	std::printf("session server watch tests passed\n");
	return 0;
}
