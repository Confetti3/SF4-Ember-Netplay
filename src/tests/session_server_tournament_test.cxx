// The session server binding the room it leads to a tournament match: the
// change goes through a recovery checkpoint like any other, strangers are sent
// away, and only the bound endpoints can join afterwards.
#include "../session/sf4e__SessionServer.hxx"

#include <array>
#include <map>
#include <stdexcept>
#include <string>

#include "server_transport_support.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

static std::string Endpoint(session::Connection connection) {
	return std::string(64, "0abcdef"[static_cast<std::size_t>(connection) % 7]);
}

static void TestBindingThroughTheServer() {
	auto* transport = new MockTransport();
	SessionServer server("tournament", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> roomId = {};
	roomId[0] = 77;
	server.EnableMatchAuthorization(roomId, [](session::Connection connection) { return Endpoint(connection); });
	server.EnableCustomRooms("Match", 16, 51);
	std::uint64_t request = 1, revision = 0;
	constexpr std::uint64_t term = 3;
	server.SetAuthority(term, revision, true);
	const auto commit = [&]() {
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		const auto proposal = server.PendingProposal();
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
	};
	const auto admit = [&](session::Connection connection) {
		protocol::SessionJoinRequest join;
		join.username = "Fighter-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
		join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello; hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
	};
	admit(1); commit();
	admit(2); commit();
	admit(3); commit();
	CHECK(server.RoomSnapshot()->members.size() == 3);

	room::TournamentBinding binding;
	binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	binding.assignmentGeneration = 1;
	binding.bindingRevision = 1;
	binding.gamesToWin = 3;
	binding.fighters[0] = {Endpoint(1), "emb1_one"};
	binding.fighters[1] = {Endpoint(2), "emb1_two"};
	transport->outgoing.clear();
	// A quorum round in progress makes the server try again later.
	admit(4);
	CHECK(!server.BindTournament(binding));
	commit();
	transport->outgoing.clear();
	CHECK(server.BindTournament(binding));
	commit();
	const auto& bound = *server.RoomSnapshot();
	CHECK(bound.tournament.Active() && bound.tournament.gamesToWin == 3);
	CHECK(bound.members.size() == 2);
	CHECK(bound.tables[room::TournamentTable].p1 && bound.tables[room::TournamentTable].p2);
	CHECK(bound.tables[room::TournamentTable].rules.format == room::SetFormat::Ft3);
	// The strangers were told they were removed, and everyone saw the room.
	bool removed = false, broadcast = false;
	for (const auto& sent : transport->outgoing) {
		if ((sent.first == 3 || sent.first == 4) && sent.second.value("type", std::string()) == "room_result" &&
			sent.second.at("result").value("reason", 0) == static_cast<int>(room::RejectReason::MemberKicked)) removed = true;
		if (sent.first == 2 && sent.second.value("type", std::string()) == "room_snapshot") broadcast = true;
	}
	CHECK(removed && broadcast);
	// A stranger cannot join a bound room.
	transport->outgoing.clear();
	admit(5);
	CHECK(server.RoomSnapshot()->members.size() == 2);
	// An older binding is refused.
	auto older = binding;
	older.bindingRevision = 0;
	CHECK(!server.BindTournament(older));
}

// A writable server leading a room bound to a tournament match between
// connections 1 and 2. Every change is committed unless a test says not to.
struct BoundServer {
	MockTransport* transport = new MockTransport();
	SessionServer server{"tournament", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	const std::uint64_t term = 3;
	std::uint64_t request = 1, revision = 0, actionId = 0;

	BoundServer() {
		std::array<std::uint8_t, 16> roomId = {};
		roomId[0] = 78;
		server.EnableMatchAuthorization(roomId, [](session::Connection connection) { return Endpoint(connection); });
		server.EnableCustomRooms("Match", 16, 51);
		server.SetAuthority(term, revision, true);
		for (const session::Connection connection : {session::Connection(1), session::Connection(2)}) {
			protocol::SessionJoinRequest join;
			join.username = "Fighter-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
			join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
			protocol::SessionHelloMsg hello; hello.admission = json(join);
			transport->Push(connection, json(hello));
			CHECK(server.Step() == 0);
			Commit();
		}
		room::TournamentBinding binding;
		binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
		binding.assignmentGeneration = 1;
		binding.bindingRevision = 1;
		binding.gamesToWin = 3;
		binding.fighters[0] = {Endpoint(1), "emb1_one"};
		binding.fighters[1] = {Endpoint(2), "emb1_two"};
		CHECK(server.BindTournament(binding));
		Commit();
	}
	void Commit() {
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		const auto proposal = server.PendingProposal();
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
	}
	void Send(session::Connection by, room::ActionKind kind, bool commit = true, std::uint64_t generation = 0) {
		const auto& snapshot = server._roomAuthority->SnapshotView();
		room::Action action;
		action.kind = kind; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
		action.table = room::TournamentTable; action.tableRevision = snapshot.tables[room::TournamentTable].revision;
		action.actionId = ++actionId;
		action.matchGeneration = generation ? generation : snapshot.tables[room::TournamentTable].permitGeneration;
		action.text = "per_one";
		action.startWindowMs = room::PermitStartMs;
		protocol::RoomActionMessage message; message.action = action;
		transport->Push(by, json(message));
		CHECK(server.Step() == 0);
		if (commit) Commit();
	}
	// Both fighters acknowledge a step of the native preparation; whatever it
	// leads to is committed.
	void Acknowledge(const char* type, std::uint64_t generation) {
		for (const session::Connection connection : {session::Connection(1), session::Connection(2)}) {
			transport->Push(connection, json{{"type", type}, {"generation", generation}});
			CHECK(server.Step() == 0);
			if (server.HasRecoveryCandidate()) Commit();
		}
	}
	// Coordination lost for lengthMs from nowMs; returns the time it is back.
	std::uint64_t Outage(std::uint64_t nowMs, std::uint64_t lengthMs) {
		server.SetAuthority(term, revision, false, false);
		server.AdvanceCustomRoom(nowMs + 500);
		server.AdvanceCustomRoom(nowMs + 500 + lengthMs);
		server.SetAuthority(term, revision, true);
		server.AdvanceCustomRoom(nowMs + 1000 + lengthMs);
		return nowMs + 1000 + lengthMs;
	}
	// How many of the messages sent since the last clear were `type` for `generation`.
	std::size_t Sent(const char* type, std::uint64_t generation) const {
		return static_cast<std::size_t>(std::count_if(transport->outgoing.begin(), transport->outgoing.end(), [&](const auto& sent) {
			return sent.second.value("type", std::string()) == type && sent.second.value("generation", std::uint64_t(0)) == generation;
		}));
	}
	const room::Table& Table() const { return server._roomAuthority->SnapshotView().tables[room::TournamentTable]; }
	const room::RoomAuthority::PermitTimer& Timer() const { return server._roomAuthority->PermitAges().tables[room::TournamentTable]; }
	// Both ready at 1000; the first permit 30 s later. Returns the generation.
	std::uint64_t HoldOnePermit() {
		server.AdvanceCustomRoom(1000);
		Send(1, room::ActionKind::Ready);
		Send(2, room::ActionKind::Ready);
		const auto reserved = Table().permitGeneration;
		CHECK(reserved != 0);
		server.AdvanceCustomRoom(31000);
		Send(1, room::ActionKind::PermitReady);
		return reserved;
	}
};

// A permit's window keeps running while this server is not writable, whether
// its coordination is healthy or not. After a long outage the owner calls the
// start off; after a short one the start still waits for the other permit.
static void TestPermitWindowThroughAnOutage() {
	for (const std::uint64_t outage : {std::uint64_t(100000), std::uint64_t(20000)}) {
		BoundServer bound;
		auto& server = bound.server;
		const auto reserved = bound.HoldOnePermit();
		// Coordination is lost, and the outage passes with no healthy time.
		server.SetAuthority(bound.term, bound.revision, false, false);
		server.AdvanceCustomRoom(31500);
		server.AdvanceCustomRoom(31500 + outage);
		CHECK(!server.HasRecoveryCandidate());
		// Writable again: the room's timer work runs on the next tick.
		server.SetAuthority(bound.term, bound.revision, true);
		server.AdvanceCustomRoom(32000 + outage);
		const auto& table = bound.Table();
		if (outage == 100000) {
			CHECK(server.HasRecoveryCandidate());
			CHECK(table.phase == room::TablePhase::Waiting && table.permitGeneration == 0);
			bound.Commit();
		} else {
			CHECK(!server.HasRecoveryCandidate());
			CHECK(table.phase == room::TablePhase::Ready && table.permitGeneration == reserved && table.permits[0] == "per_one");
		}
	}
}

// A retried recovery restores the same commit tick after tick while its
// members are not bound yet. The permit ages on across every restore, and
// the owner that finally takes over calls the late start off.
static void TestRepeatedRestoresKeepPermitAge() {
	BoundServer bound;
	auto& server = bound.server;
	bound.HoldOnePermit();
	const auto committed = server.RecoveryCheckpoint();
	for (std::uint64_t nowMs = 61000; nowMs <= 151000; nowMs += 30000) {
		CHECK(server.RestoreRecoveryCheckpoint(committed));
		server.SetAuthority(bound.term, bound.revision, false, false);
		server.AdvanceCustomRoom(nowMs);
		CHECK(bound.Timer().ageMs == nowMs - 1000);
	}
	server.SetAuthority(bound.term, bound.revision, true);
	server.AdvanceCustomRoom(161500);
	CHECK(bound.Table().phase == room::TablePhase::Waiting && bound.Table().permitGeneration == 0);
}

// The permit window holds until the native start. A game begun inside it
// whose game_prepare commits only after an outage, past the window or past
// the bridge's SILENT_SECS (30 minutes) after it, or whose fighters are
// ready only once the window has run out, is called off: game_end, a Cancel
// result, and no game_start.
static void TestLateNativeStartIsCalledOff() {
	struct Late { std::uint64_t beforePrepare, beforeReady; };
	for (const auto late : {Late{100000, 0}, Late{1800000 + 120000, 0}, Late{0, 100000}}) {
		BoundServer bound;
		auto& server = bound.server;
		const auto reserved = bound.HoldOnePermit();
		// The second permit begins the game in a candidate an outage holds back.
		bound.Send(2, room::ActionKind::PermitReady, false);
		CHECK(bound.Table().phase == room::TablePhase::Playing && bound.Table().matchGeneration == reserved);
		auto nowMs = bound.Outage(31000, late.beforePrepare);
		bound.transport->outgoing.clear();
		bound.Commit();
		CHECK(bound.Sent("game_prepare", reserved) == 2);
		bound.Acknowledge("game_prepared", reserved);
		if (late.beforeReady) {
			CHECK(bound.Sent("game_connect", reserved) == 2);
			nowMs = bound.Outage(nowMs, late.beforeReady);
		}
		bound.Acknowledge("game_ready", reserved);
		CHECK(bound.Sent("game_end", reserved) == 2 && bound.Sent("game_start", reserved) == 0);
		const auto& table = bound.Table();
		CHECK(table.phase == room::TablePhase::Waiting && table.matchGeneration == reserved && bound.Timer().generation == 0);
		const auto receipts = server._roomAuthority->PendingTerminalEvents(table.p1);
		CHECK(receipts.size() == 1 && receipts[0].generation == reserved && receipts[0].result == room::MatchResult::Cancel);
	}
}

// The accepted bound: a game_start proposed inside the window but committed
// only after a same-term outage, here past SILENT_SECS, still starts the
// game. Its delay is that one proposal's commit latency.
static void TestStartCommittedLateStillStarts() {
	BoundServer bound;
	auto& server = bound.server;
	const auto reserved = bound.HoldOnePermit();
	bound.Send(2, room::ActionKind::PermitReady);
	bound.Acknowledge("game_prepared", reserved);
	bound.transport->Push(1, json{{"type", "game_ready"}, {"generation", reserved}});
	CHECK(server.Step() == 0);
	if (server.HasRecoveryCandidate()) bound.Commit();
	bound.transport->Push(2, json{{"type", "game_ready"}, {"generation", reserved}});
	CHECK(server.Step() == 0 && server.HasRecoveryCandidate());
	CHECK(bound.Timer().generation == 0);
	// Proposed inside the window, and held pending through the outage.
	CHECK(server.ProposeCheckpoint(bound.request, bound.term, bound.revision, nullptr));
	const auto proposal = server.PendingProposal();
	CHECK(proposal != nullptr);
	bound.transport->outgoing.clear();
	bound.Outage(31000, 1800000 + 120000);
	CHECK(server.PendingProposal() == proposal && bound.Sent("game_start", reserved) == 0);
	CHECK(server.ApplyCommit(proposal->request, bound.term, bound.revision + 1, proposal->checkpoint, proposal->effectsDigest));
	++bound.request; ++bound.revision;
	CHECK(bound.Sent("game_start", reserved) == 2 && bound.Sent("game_end", reserved) == 0);
	CHECK(bound.Table().phase == room::TablePhase::Playing && bound.Table().matchGeneration == reserved);
}

// A result reported during the preparation pauses the table over it, but
// the game has still not natively started: the permit window holds through
// the pause and through a checkpoint the next owner restores, and the
// acknowledgements that come after it ran out call the game off.
static void TestPausedPreparationKeepsWindow() {
	BoundServer bound;
	auto& server = bound.server;
	const auto reserved = bound.HoldOnePermit();
	bound.Send(2, room::ActionKind::PermitReady);
	CHECK(bound.Sent("game_prepare", reserved) == 2);
	bound.Send(1, room::ActionKind::MatchFinished, true, reserved);
	server.AdvanceCustomRoom(61500);
	if (server.HasRecoveryCandidate()) bound.Commit();
	CHECK(bound.Table().phase == room::TablePhase::Paused && bound.Table().matchGeneration == reserved);
	std::map<room::MemberId, session::Connection> connections;
	for (const auto& row : server.roomMembers) connections[row.second] = row.first;
	const auto committed = server.RecoveryCheckpoint();
	CHECK(committed.at("room").contains("start_window") && committed.at("room").at("start_window").at(0) == room::PermitStartMs);
	CHECK(server.RestoreRecoveryCheckpoint(committed));
	std::vector<SessionServer::StableRebind> bindings;
	for (const auto& row : committed.at("members")) {
		const auto member = row.at("member").get<room::MemberId>();
		bindings.emplace_back(member, connections.at(member), row.at("data").get<protocol::MemberData>().connId,
			row.at("incarnation").get<std::uint64_t>());
	}
	CHECK(server.RebindMembers(bindings));
	server.SetAuthority(bound.term, bound.revision, true);
	server.AdvanceCustomRoom(122000);
	CHECK(bound.Table().phase == room::TablePhase::Paused && bound.Timer().windowMs == room::PermitStartMs);
	bound.transport->outgoing.clear();
	bound.Acknowledge("game_prepared", reserved);
	bound.Acknowledge("game_ready", reserved);
	CHECK(bound.Sent("game_end", reserved) == 2 && bound.Sent("game_start", reserved) == 0);
	const auto& table = bound.Table();
	CHECK(table.phase == room::TablePhase::Waiting && bound.Timer().generation == 0);
	const auto receipts = server._roomAuthority->PendingTerminalEvents(table.p1);
	CHECK(receipts.size() == 1 && receipts[0].generation == reserved && receipts[0].result == room::MatchResult::Cancel);
}

// A candidate that starts the game, takes a Ready back or lets the hold run
// out clears the reservation. Rolled back, by a discarded proposal or a new
// term, the reservation is as old as this server has seen it, not as old as
// the baseline's commit says: 30 s old at 31 s when the candidate opens, it
// is 160 s old at 161 s and is called off.
static void TestRollbackKeepsPermitAge() {
	for (int way = 0; way < 4; ++way) {
		BoundServer bound;
		auto& server = bound.server;
		const auto reserved = bound.HoldOnePermit();
		if (way == 0) bound.Send(2, room::ActionKind::PermitReady, false);
		else if (way == 1 || way == 3) bound.Send(2, room::ActionKind::Unready, false);
		else server.AdvanceCustomRoom(31000 + room::PermitHoldMs);
		CHECK(server.HasRecoveryCandidate() && bound.Table().permitGeneration == 0);
		CHECK((bound.Table().phase == room::TablePhase::Playing) == (way == 0));
		if (way == 3) server.SetAuthority(bound.term + 1, bound.revision, true);
		else server.DiscardProposal();
		CHECK(!server.HasRecoveryCandidate() && bound.Table().permitGeneration == reserved);
		CHECK(bound.Timer().ageMs == 30000 && bound.Timer().sampled && bound.Timer().sampleMs == 31000);
		server.AdvanceCustomRoom(161000);
		CHECK(bound.Table().phase == room::TablePhase::Waiting && bound.Table().permitGeneration == 0);
	}
}

// A candidate's own reservation is not committed, so a successor may reserve
// the same generation. Replaced by the successor's commit, after a rollback
// with no tick between or while the candidate is still open, the successor's
// reservation keeps its own age, whatever the candidate's had reached.
static void TestCandidateReservationIsNotKept() {
	for (const bool discard : {true, false}) {
		BoundServer bound;
		auto& server = bound.server;
		server.AdvanceCustomRoom(1000);
		bound.Send(1, room::ActionKind::Ready);
		bound.Send(2, room::ActionKind::Ready, false);
		const auto speculative = bound.Table().permitGeneration;
		CHECK(speculative != 0);
		// The successor reserved the same generation 1 s before its commit.
		auto successor = server.RecoveryCheckpoint();
		successor["room"]["permit_age"][0] = std::uint64_t(1000);
		// Not writable, the candidate's reservation ages on.
		server.SetAuthority(bound.term, bound.revision, false, false);
		server.AdvanceCustomRoom(100000);
		CHECK(bound.Timer().generation == speculative && bound.Timer().ageMs == 99000);
		if (discard) server.DiscardProposal();
		CHECK(server.RestoreRecoveryCheckpoint(successor));
		CHECK(bound.Table().permitGeneration == speculative);
		CHECK(bound.Timer().ageMs == 1000 && !bound.Timer().sampled);
	}
}

int main() {
	TestBindingThroughTheServer();
	TestPermitWindowThroughAnOutage();
	TestRepeatedRestoresKeepPermitAge();
	TestLateNativeStartIsCalledOff();
	TestStartCommittedLateStillStarts();
	TestPausedPreparationKeepsWindow();
	TestRollbackKeepsPermitAge();
	TestCandidateReservationIsNotKept();
	std::printf("session server tournament tests passed\n");
	return 0;
}
