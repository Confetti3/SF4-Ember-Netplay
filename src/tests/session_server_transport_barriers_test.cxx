#include "session_server_transport_support.hxx"

// Spectators hold back the fighters' start only when P1 has not accepted the
// authority's optional-spectator offer (an older P1 client).
static void TestOptionalSpectatorBarrier() {
	std::array<std::uint8_t, 16> room = {};
	room[0] = 9;
	const auto identity = [](session::Connection connection) {
		std::string value(64, '0');
		value[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return value;
	};
	const std::vector<session::MatchAuthority::Participant> participants = {
		{1, {"relay", "p1"}}, {2, {"relay", "p2"}}, {3, {"relay", "s1"}}, {4, {"relay", "s2"}}};
	std::vector<std::pair<session::Connection, json>> sent;
	const session::MatchAuthority::Send send = [&](session::Connection connection, const json& message) {
		sent.emplace_back(connection, message); return true;
	};
	const auto ack = [](session::Connection connection, const char* type, json extra = json::object()) {
		extra["type"] = type; extra["generation"] = 12; return extra;
	};
	const auto count = [&](const char* type, session::Connection connection) {
		return std::count_if(sent.begin(), sent.end(), [&](const std::pair<session::Connection, json>& item) {
			return item.first == connection && item.second.at("type") == type;
		});
	};
	using Phase = session::MatchAuthority::Phase;
	{
		// An older P1 does not echo the offer: every participant still gates.
		session::MatchAuthority authority(room, identity);
		CHECK(authority.BeginAtGeneration(participants, 12, send));
		for (const auto& item : sent) CHECK(item.second.at("spectators_optional") == true);
		CHECK(authority.Acknowledge(1, ack(1, "game_prepared"), send));
		CHECK(authority.Acknowledge(2, ack(2, "game_prepared"), send));
		CHECK(authority.GetPhase() == Phase::Preparing);
		CHECK(authority.Acknowledge(3, ack(3, "game_prepared"), send));
		CHECK(authority.Acknowledge(4, ack(4, "game_prepared"), send));
		CHECK(authority.GetPhase() == Phase::Connecting);
		// And a spectator leaving before the start still ends the setup.
		CHECK(authority.MemberDeparted(4, send));
		CHECK(authority.GetPhase() == Phase::Idle);
	}
	sent.clear();
	{
		session::MatchAuthority authority(room, identity);
		CHECK(authority.BeginAtGeneration(participants, 12, send));
		CHECK(authority.Acknowledge(1, ack(1, "game_prepared", {{"spectators_optional", true}}), send));
		CHECK(authority.Acknowledge(2, ack(2, "game_prepared"), send));
		// The fighters alone open Connecting; the spectators are told too.
		CHECK(authority.GetPhase() == Phase::Connecting);
		for (session::Connection connection = 1; connection <= 4; ++connection) CHECK(count("game_connect", connection) == 1);
		CHECK(authority.Acknowledge(3, ack(3, "game_ready"), send));
		CHECK(authority.Acknowledge(2, ack(2, "game_ready"), send));
		CHECK(authority.GetPhase() == Phase::Connecting); // P1 has not reported yet

		// A successor mid-Connecting keeps the agreement and the partial acks.
		const auto portable = authority.PortableCheckpoint();
		session::MatchAuthority successor(room, identity);
		CHECK(successor.RestorePortableCheckpoint(portable, [&](const protocol::ConnectionID& member) {
			for (const auto& participant : participants) if (participant.member == member) return participant.connection;
			return session::Connection(0);
		}));
		CHECK(successor.PortableCheckpoint() == portable);

		// P1 reports only s1's link up: s2 sits this generation out.
		CHECK(successor.Acknowledge(1, ack(1, "game_ready", {{"slots", json::array({2})}}), send));
		CHECK(successor.GetPhase() == Phase::Started);
		CHECK(count("game_start", 1) == 1 && count("game_start", 2) == 1 && count("game_start", 3) == 1);
		CHECK(count("game_start", 4) == 0 && count("game_end", 4) == 1);
		// The dropped spectator is departed, so a rebind need not name it.
		CHECK(successor.RebindConnections({{{"relay", "p1"}, 1}, {{"relay", "p2"}, 2}, {{"relay", "s1"}, 3}}));
	}
	sent.clear();
	{
		// A spectator leaving before the start no longer ends the fighters' setup.
		session::MatchAuthority authority(room, identity);
		CHECK(authority.BeginAtGeneration(participants, 12, send));
		CHECK(authority.Acknowledge(1, ack(1, "game_prepared", {{"spectators_optional", true}}), send));
		CHECK(authority.Acknowledge(2, ack(2, "game_prepared"), send));
		CHECK(authority.MemberDeparted(4, send));
		CHECK(authority.GetPhase() == Phase::Connecting);
		CHECK(std::any_of(sent.begin(), sent.end(), [](const std::pair<session::Connection, json>& item) {
			return item.first == 1 && item.second.at("type") == "game_peer_end" && item.second.at("slot") == 3;
		}));
		CHECK(authority.Acknowledge(2, ack(2, "game_ready"), send));
		CHECK(authority.Acknowledge(1, ack(1, "game_ready", {{"slots", json::array({2})}}), send));
		CHECK(authority.GetPhase() == Phase::Started);
		CHECK(count("game_start", 4) == 0);
		// The live roster is the fighters and the spectator that started.
		CHECK((authority.LiveParticipants() == std::vector<session::Connection>{1, 2, 3}));
		CHECK(authority.IsLiveParticipant(3) && !authority.IsLiveParticipant(4) && !authority.IsLiveParticipant(9));
		CHECK(authority.Fighter(0) == 1 && authority.Fighter(1) == 2 && authority.Fighter(2) == 0);
		CHECK(authority.End(send));
		CHECK(authority.LiveParticipants().empty() && !authority.IsLiveParticipant(1) && authority.Fighter(0) == 0);
	}
	sent.clear();
	{
		// A spectator that leaves before P1 has taken up the generation cannot be
		// told to P1 yet (P1 drops a peer end for a generation it has not adopted),
		// and ends nothing. P1 hears of it with its game_prepared.
		session::MatchAuthority authority(room, identity);
		CHECK(authority.BeginAtGeneration(participants, 12, send));
		CHECK(authority.MemberDeparted(4, send));
		CHECK(authority.GetPhase() == Phase::Preparing);
		for (session::Connection connection = 1; connection <= 4; ++connection) CHECK(count("game_end", connection) == 0);
		CHECK(count("game_peer_end", 1) == 0);
		CHECK(!authority.IsLiveParticipant(4));
		CHECK(authority.Acknowledge(1, ack(1, "game_prepared", {{"spectators_optional", true}}), send));
		CHECK(std::any_of(sent.begin(), sent.end(), [](const std::pair<session::Connection, json>& item) {
			return item.first == 1 && item.second.at("type") == "game_peer_end" && item.second.at("slot") == 3;
		}));
		CHECK(authority.Acknowledge(2, ack(2, "game_prepared"), send));
		CHECK(authority.GetPhase() == Phase::Connecting);
		for (session::Connection connection = 1; connection <= 3; ++connection) CHECK(count("game_connect", connection) == 1);
		CHECK(count("game_connect", 4) == 0);
	}
	sent.clear();
	{
		// A P1 that does not take up optional spectators still needs every one of
		// them, so the departure ends the setup when it acknowledges.
		session::MatchAuthority authority(room, identity);
		CHECK(authority.BeginAtGeneration(participants, 12, send));
		CHECK(authority.MemberDeparted(4, send));
		CHECK(authority.GetPhase() == Phase::Preparing);
		CHECK(authority.Acknowledge(1, ack(1, "game_prepared"), send));
		CHECK(authority.GetPhase() == Phase::Idle);
	}
}

// A spectator still retiring the last generation is left out of the next
// grant. Every native projection of that generation must leave it out too,
// or the fighters' AcceptGrant rejects the rematch as invalid_match_roster.
static void TestRetiringSpectatorProjection() {
	auto* transport = new MockTransport();
	SessionServer server("retiring-spectator", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> authorizationRoom = {};
	authorizationRoom[0] = 75;
	server.EnableMatchAuthorization(authorizationRoom, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Retiring spectator", 4, 81);
	CHECK(server.Listen(0) == 0);
	for (session::Connection connection = 1; connection <= 3; ++connection) {
		protocol::SessionJoinRequest join;
		join.username = "Retire-" + std::to_string(connection);
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(35000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
	}
	std::uint64_t nextAction = 0;
	const auto act = [&](session::Connection connection, room::ActionKind kind, std::uint64_t generation = 0) {
		room::Action action;
		const auto* snapshot = server.RoomSnapshot();
		action.kind = kind;
		action.roomEpoch = snapshot->roomEpoch;
		action.revision = snapshot->revision;
		action.table = 0;
		action.tableRevision = snapshot->tables[0].revision;
		action.matchGeneration = generation;
		action.actionId = ++nextAction;
		protocol::RoomActionMessage message;
		message.action = action;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
	};
	const auto begin = [&]() {
		transport->outgoing.clear();
		act(1, room::ActionKind::Ready); act(2, room::ActionKind::Ready);
		CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
		return server.RoomSnapshot()->tables[0].matchGeneration;
	};
	// Every projection of this generation lists exactly the grant roster.
	const auto checkProjections = [&](std::uint64_t generation, std::size_t rosterSize) {
		std::size_t grants = 0, projections = 0;
		for (const auto& sent : transport->outgoing) {
			const auto type = sent.second.value("type", std::string());
			if (type == "game_prepare") {
				CHECK(sent.second.at("roster").size() == rosterSize);
				++grants;
			} else if (type == "data_update" && sent.second.value("matchGeneration", std::uint64_t(0)) == generation) {
				CHECK(sent.second.at("lobbyData").at("members").size() == rosterSize);
				++projections;
			}
		}
		CHECK(grants == rosterSize && projections >= rosterSize);
	};
	act(1, room::ActionKind::Queue); act(2, room::ActionKind::Queue); act(3, room::ActionKind::Watch);
	const auto first = begin();
	checkProjections(first, 3);
	act(1, room::ActionKind::AbortMatch, first);
	CHECK(server.RoomSnapshot()->tables[0].phase != room::TablePhase::Playing);
	act(1, room::ActionKind::AcknowledgeTerminal, first);
	act(2, room::ActionKind::AcknowledgeTerminal, first);
	// Connection 3 has not acknowledged generation one.
	const auto second = begin();
	CHECK(second > first);
	checkProjections(second, 2);
	// A later broadcast in the same generation keeps the frozen roster.
	transport->outgoing.clear();
	act(3, room::ActionKind::AcknowledgeTerminal, first);
	for (const auto& sent : transport->outgoing)
		if (sent.second.value("type", std::string()) == "data_update" && sent.first != 3 &&
			sent.second.value("matchGeneration", std::uint64_t(0)) == second)
			CHECK(sent.second.at("lobbyData").at("members").size() == 2);
}

// The native load barrier (battle_loaded / battle_synced) and punch barrier
// belong to one generation: they are built from its roster, the fighters gate
// them, nothing carries over from the last game, and a spectator that loads
// after the fighters is released at once.
namespace {
struct BarrierRoom {
	MockTransport* transport = new MockTransport();
	SessionServer server;
	std::uint64_t nextAction = 0;

	BarrierRoom(const char* name, std::uint8_t seed, int members)
		: server(name, "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)) {
		std::array<std::uint8_t, 16> authorizationRoom = {};
		authorizationRoom[0] = seed;
		server.EnableMatchAuthorization(authorizationRoom, [](session::Connection connection) {
			std::string identity(64, '0');
			identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
			return identity;
		});
		server.EnableCustomRooms(name, 4, 90 + seed);
		CHECK(server.Listen(0) == 0);
		for (session::Connection connection = 1; connection <= static_cast<session::Connection>(members); ++connection) {
			protocol::SessionJoinRequest join;
			join.username = std::string(name) + "-" + std::to_string(connection);
			join.sidecarHash = "build";
			join.port = static_cast<std::uint16_t>(36000 + connection);
			join.customRooms = true;
			join.roomProtocol = room::ProtocolVersion;
			protocol::SessionHelloMsg hello;
			hello.admission = json(join);
			transport->Push(connection, json(hello));
			CHECK(server.Step() == 0);
		}
	}
	void Act(session::Connection connection, room::ActionKind kind, std::uint64_t generation = 0) {
		room::Action action;
		const auto* snapshot = server.RoomSnapshot();
		action.kind = kind;
		action.roomEpoch = snapshot->roomEpoch;
		action.revision = snapshot->revision;
		action.table = 0;
		action.tableRevision = snapshot->tables[0].revision;
		action.matchGeneration = generation;
		action.actionId = ++nextAction;
		protocol::RoomActionMessage message;
		message.action = action;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
	}
	void Send(session::Connection connection, const char* type, std::uint64_t generation = 0, json extra = json::object()) {
		extra["type"] = type;
		if (generation) extra["generation"] = generation;
		transport->Push(connection, extra);
		CHECK(server.Step() == 0);
	}
	bool Got(const char* type, session::Connection connection) const {
		return std::any_of(transport->outgoing.begin(), transport->outgoing.end(), [&](const auto& sent) {
			return sent.first == connection && sent.second.value("type", std::string()) == type;
		});
	}
	// Starts a game for the queued fighters (connections 1 and 2) and takes it to
	// Started. `startSlots` is the spectator slots P1 reports up.
	std::uint64_t Start(std::uint64_t spectators, const json& startSlots) {
		transport->outgoing.clear();
		Act(1, room::ActionKind::Ready); Act(2, room::ActionKind::Ready);
		CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
		const auto generation = server.RoomSnapshot()->tables[0].matchGeneration;
		Send(1, "game_prepared", generation, {{"spectators_optional", true}});
		Send(2, "game_prepared", generation);
		for (session::Connection spectator = 3; spectator < 3 + spectators; ++spectator) Send(spectator, "game_prepared", generation);
		Send(1, "game_ready", generation, {{"slots", startSlots}});
		Send(2, "game_ready", generation);
		for (session::Connection spectator = 3; spectator < 3 + spectators; ++spectator) Send(spectator, "game_ready", generation);
		return generation;
	}
	void Loaded(session::Connection connection) { transport->outgoing.clear(); Send(connection, "battle_loaded"); }
};
}

static void TestRoomBattleBarrier() {
	{
		// Two games at one table with a spectator live in both.
		BarrierRoom fixture("barrier-rematch", 1, 3);
		fixture.Act(1, room::ActionKind::Queue); fixture.Act(2, room::ActionKind::Queue); fixture.Act(3, room::ActionKind::Watch);
		const auto first = fixture.Start(1, json::array({2}));
		fixture.Loaded(1);
		CHECK(!fixture.Got("battle_synced", 1) && !fixture.Got("battle_synced", 3));
		fixture.Loaded(2);
		CHECK(fixture.Got("battle_synced", 1) && fixture.Got("battle_synced", 2));
		// The spectator loads after the fighters and is released at once.
		fixture.Loaded(3);
		CHECK(fixture.Got("battle_synced", 3) && !fixture.Got("battle_synced", 1));
		fixture.Act(1, room::ActionKind::AbortMatch, first);
		for (session::Connection connection = 1; connection <= 3; ++connection) fixture.Act(connection, room::ActionKind::AcknowledgeTerminal, first);
		const auto second = fixture.Start(1, json::array({2}));
		CHECK(second > first);
		// Nothing loaded last game counts for this one: the first fighter waits.
		fixture.Loaded(1);
		CHECK(!fixture.Got("battle_synced", 1) && !fixture.Got("battle_synced", 2) && !fixture.Got("battle_synced", 3));
		fixture.Loaded(2);
		CHECK(fixture.Got("battle_synced", 1) && fixture.Got("battle_synced", 2) && !fixture.Got("battle_synced", 3));
		fixture.Loaded(3);
		CHECK(fixture.Got("battle_synced", 3));
		// The punch barrier is scoped the same way: the fighters gate it.
		fixture.Loaded(1);
		fixture.transport->outgoing.clear(); fixture.Send(1, "punch_ready");
		CHECK(!fixture.Got("punch_go", 1));
		fixture.Send(2, "punch_ready");
		CHECK(fixture.Got("punch_go", 1) && fixture.Got("punch_go", 2));
	}
	{
		// A spectator whose link was not up at the start is dropped from the
		// generation. It never loads, and it never holds the fighters.
		BarrierRoom fixture("barrier-late", 2, 3);
		fixture.Act(1, room::ActionKind::Queue); fixture.Act(2, room::ActionKind::Queue); fixture.Act(3, room::ActionKind::Watch);
		fixture.transport->outgoing.clear();
		fixture.Act(1, room::ActionKind::Ready); fixture.Act(2, room::ActionKind::Ready);
		const auto generation = fixture.server.RoomSnapshot()->tables[0].matchGeneration;
		fixture.Send(1, "game_prepared", generation, {{"spectators_optional", true}});
		fixture.Send(2, "game_prepared", generation);
		fixture.Send(1, "game_ready", generation, {{"slots", json::array()}});
		fixture.Send(2, "game_ready", generation);
		CHECK(fixture.Got("game_end", 3) && fixture.Got("game_start", 1) && fixture.Got("game_start", 2));
		// The dropped spectator is still listed at the table.
		CHECK(fixture.server.RoomSnapshot()->tables[0].spectators.size() == 1);
		fixture.Loaded(1);
		CHECK(!fixture.Got("battle_synced", 1));
		fixture.Loaded(2);
		CHECK(fixture.Got("battle_synced", 1) && fixture.Got("battle_synced", 2) && !fixture.Got("battle_synced", 3));
		// It cannot join a barrier of a generation that left it out.
		fixture.Loaded(3);
		CHECK(!fixture.Got("battle_synced", 3));
	}
	{
		// A spectator still retiring the last game stays in the table's list but
		// out of the next generation, and so out of its barrier.
		BarrierRoom fixture("barrier-retiring", 3, 3);
		fixture.Act(1, room::ActionKind::Queue); fixture.Act(2, room::ActionKind::Queue); fixture.Act(3, room::ActionKind::Watch);
		const auto first = fixture.Start(1, json::array({2}));
		fixture.Act(1, room::ActionKind::AbortMatch, first);
		fixture.Act(1, room::ActionKind::AcknowledgeTerminal, first);
		fixture.Act(2, room::ActionKind::AcknowledgeTerminal, first);
		const auto second = fixture.Start(0, json::array());
		CHECK(second > first);
		CHECK(fixture.server.RoomSnapshot()->tables[0].spectators.size() == 1);
		fixture.Loaded(1);
		fixture.Loaded(2);
		CHECK(fixture.Got("battle_synced", 1) && fixture.Got("battle_synced", 2));
	}
}

// A spectator that leaves before P1 has taken up the generation waits for P1's
// game_prepared to be told. It ends no game.
static void TestSpectatorLeavesBeforeP1Prepared() {
	BarrierRoom fixture("early-leave", 4, 3);
	fixture.Act(1, room::ActionKind::Queue); fixture.Act(2, room::ActionKind::Queue); fixture.Act(3, room::ActionKind::Watch);
	fixture.transport->outgoing.clear();
	fixture.Act(1, room::ActionKind::Ready); fixture.Act(2, room::ActionKind::Ready);
	const auto generation = fixture.server.RoomSnapshot()->tables[0].matchGeneration;
	fixture.transport->outgoing.clear();
	fixture.Act(3, room::ActionKind::Unwatch);
	CHECK(fixture.server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(!fixture.Got("game_end", 1) && !fixture.Got("game_end", 2) && !fixture.Got("game_peer_end", 1));
	// P1 takes up the generation and only now hears the spectator is gone.
	fixture.transport->outgoing.clear();
	fixture.Send(1, "game_prepared", generation, {{"spectators_optional", true}});
	CHECK(fixture.Got("game_peer_end", 1));
	fixture.transport->outgoing.clear();
	fixture.Send(2, "game_prepared", generation);
	CHECK(fixture.Got("game_connect", 1) && fixture.Got("game_connect", 2) && !fixture.Got("game_connect", 3));
	fixture.Send(1, "game_ready", generation, {{"slots", json::array()}});
	fixture.Send(2, "game_ready", generation);
	CHECK(fixture.server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(fixture.Got("game_start", 1) && fixture.Got("game_start", 2) && !fixture.Got("game_start", 3));
}

int main() {
	TestOptionalSpectatorBarrier();
	TestRetiringSpectatorProjection();
	TestRoomBattleBarrier();
	TestSpectatorLeavesBeforeP1Prepared();
	std::cout << "Session server mock transport barriers tests passed\n";
}
