// The latency route a member's join carries: how the server admits it into the
// member's stable record, and how that record survives checkpoints, canonical
// and portable restores, rebinds and departures. Also the shared endpoint ID
// predicate and the room's own route from the helper's events.
#include "../session/sf4e__SessionServer.hxx"
#include "../session/IrohRoom.hxx"
#include "../common/EndpointIdentity.hxx"
#include "../common/LatencyRoute.hxx"
#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "test_support.hxx"
#include "server_transport_support.hxx"
#include "fake_helper_pipe.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

namespace {
const std::array<std::uint8_t, 16> kRoom{{4}};

std::string Hex64(char fill, std::size_t value) {
	std::string id(64, fill);
	id[62] = "0123456789abcdef"[(value >> 4) & 15]; id[63] = "0123456789abcdef"[value & 15];
	return id;
}
// The authenticated endpoint the helper vouches for on a connection.
std::string Primary(session::Connection connection) { return Hex64('0', static_cast<std::size_t>(connection)); }
// A latency endpoint ID, never equal to a primary one.
std::string Route(std::size_t n) { return Hex64('c', n); }
json RouteJson(const std::string& endpoint, const char* relay = nullptr) {
	json value = json::object({{"endpoint", endpoint}});
	if (relay) value["relay"] = relay;
	return value;
}

std::unique_ptr<SessionServer> Fresh(MockTransport** transport = nullptr) {
	auto* owned = new MockTransport();
	if (transport) *transport = owned;
	auto server = std::make_unique<SessionServer>("latency-room", "build", true, 3, Dimps::Math::FixedPoint{0, 99},
		std::unique_ptr<session::ServerTransport>(owned));
	server->EnableMatchAuthorization(kRoom, Primary);
	return server;
}

struct LatencyRoom {
	MockTransport* transport = new MockTransport();
	SessionServer server{"latency-room", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	std::map<session::Connection, std::uint64_t> incarnations;
	std::uint64_t actionId = 0, generation = 0;
	LatencyRoom() {
		server.EnableMatchAuthorization(kRoom, Primary, [this](session::Connection connection) {
			const auto found = incarnations.find(connection);
			return found == incarnations.end() ? std::uint64_t(1) : found->second;
		});
		server.EnableCustomRooms("Latency", 16, 9);
		CHECK(server.Listen(0) == 0);
	}
	void Step() { CHECK(server.Step() == 0); }
	static json Request(session::Connection connection, const json& latency) {
		protocol::SessionJoinRequest request;
		request.username = "Member" + std::to_string(connection); request.sidecarHash = "build"; request.port = 30000;
		request.customRooms = true; request.roomProtocol = room::ProtocolVersion;
		json value = request;
		if (!latency.is_null()) value["latency"] = latency;
		return value;
	}
	// Current clients carry the admission in their first hello.
	void HelloJoin(session::Connection connection, const json& latency) {
		protocol::SessionHelloMsg hello; hello.admission = Request(connection, latency);
		transport->Push(connection, json(hello)); Step();
	}
	// A bare hello, then the fallback join_req.
	void FallbackJoin(session::Connection connection, const json& latency) {
		transport->Push(connection, json(protocol::SessionHelloMsg())); Step();
		transport->Push(connection, Request(connection, latency)); Step();
	}
	room::MemberId Member(session::Connection connection) const { return server.roomMembers.at(connection); }
	void Act(session::Connection connection, room::ActionKind kind, room::MemberId target = 0) {
		const auto* snapshot = server.RoomSnapshot();
		protocol::RoomActionMessage message;
		message.action.kind = kind; message.action.roomEpoch = snapshot->roomEpoch; message.action.revision = snapshot->revision;
		message.action.table = 0; message.action.tableRevision = snapshot->tables[0].revision;
		message.action.actionId = ++actionId; message.action.target = target;
		transport->Push(connection, json(message)); Step();
	}
	void Acknowledge(session::Connection connection, const char* type, json extra = json::object()) {
		extra["type"] = type; extra["generation"] = generation; transport->Push(connection, extra); Step();
	}
};

// The active row of a member in a recovery checkpoint.
const json& ActiveRow(const json& checkpoint, room::MemberId member) {
	for (const auto& row : checkpoint.at("members"))
		if (!row.value("frozen", false) && row.at("member") == member) return row;
	std::cerr << "no active row for member " << member << '\n';
	std::exit(1);
}
std::string RouteOf(const json& data) { return data.at("latency").value("endpoint", std::string()); }

// What every view of one member must agree on.
struct Identity { std::string endpoint; std::uint64_t incarnation = 0; std::string route; };
using Expected = std::map<room::MemberId, Identity>;

// The emitted checkpoint rows, the incarnation map, the stable records, the
// room snapshot and the client projections all carry the expected tuples, and
// nothing else. extraIncarnations counts frozen-only entries.
void CheckIdentities(const SessionServer& server, const Expected& expected, std::size_t extraIncarnations = 0) {
	const auto checkpoint = server.RecoveryCheckpoint();
	std::size_t active = 0;
	for (const auto& row : checkpoint.at("members")) {
		if (row.value("frozen", false)) continue;
		++active;
		const auto& want = expected.at(row.at("member").get<room::MemberId>());
		const auto& data = row.at("data");
		CHECK(data.at("authenticatedEndpoint") == want.endpoint);
		CHECK(row.at("incarnation") == want.incarnation && data.at("incarnation") == want.incarnation);
		CHECK(RouteOf(data) == want.route);
		CHECK(!data.at("latency").contains("relay"));
	}
	CHECK(active == expected.size());
	CHECK(server.roomIncarnations.size() == expected.size() + extraIncarnations);
	CHECK(server.roomMemberRecords.size() == expected.size());
	for (const auto& item : expected) {
		const auto& want = item.second;
		CHECK(server.roomIncarnations.at(item.first) == want.incarnation);
		const auto& record = server.roomMemberRecords.at(item.first);
		CHECK(record.roomMember == item.first && record.authenticatedEndpoint == want.endpoint);
		CHECK(record.incarnation == want.incarnation && record.latency.endpoint == want.route);
		const auto& members = server.RoomSnapshot()->members;
		const auto member = std::find_if(members.begin(), members.end(), [&](const room::Member& m) { return m.id == item.first; });
		CHECK(member != members.end() && member->incarnation == want.incarnation);
	}
	for (const auto& client : server.clients) {
		const auto mapping = server.roomMembers.find(client.conn);
		if (mapping == server.roomMembers.end()) continue;
		const auto& want = expected.at(mapping->second);
		CHECK(client.data.incarnation == want.incarnation && client.data.latency.endpoint == want.route);
	}
}

std::vector<SessionServer::StableRebind> Bindings(const json& checkpoint, const Expected& expected, session::Connection first) {
	std::vector<SessionServer::StableRebind> bindings;
	for (const auto& row : checkpoint.at("members")) {
		if (row.value("frozen", false)) continue;
		const auto member = row.at("member").get<room::MemberId>();
		bindings.emplace_back(member, first++, row.at("data").get<protocol::MemberData>().connId, expected.at(member).incarnation);
	}
	return bindings;
}

json& LegacyRow(json& checkpoint, room::MemberId member) {
	for (auto& row : checkpoint.at("members")) if (row.at("data").at("roomMember") == member) return row;
	std::cerr << "no legacy row for member " << member << '\n';
	std::exit(1);
}
json& PortableRow(json& checkpoint, room::MemberId member) {
	for (auto& row : checkpoint.at("members")) if (!row.value("frozen", false) && row.at("member") == member) return row;
	std::cerr << "no portable row for member " << member << '\n';
	std::exit(1);
}
json& SnapshotMember(json& checkpoint, room::MemberId member) {
	for (auto& row : checkpoint.at("room").at("snapshot").at("members")) if (row.at("id") == member) return row;
	std::cerr << "no snapshot member " << member << '\n';
	std::exit(1);
}

// Three members with distinct incarnations and routes.
struct ThreeMembers {
	LatencyRoom room;
	Expected expected;
	ThreeMembers() {
		room.incarnations = {{1, 7}, {2, 19}, {3, 42}};
		for (session::Connection connection = 1; connection <= 3; ++connection) {
			room.HelloJoin(connection, RouteJson(Route(connection)));
			expected[room.Member(connection)] = {Primary(connection), room.incarnations.at(connection), Route(connection)};
		}
	}
	room::MemberId Member(session::Connection connection) const { return room.Member(connection); }
};
}

// C++ 1: a route with an ID reaches the checkpoint through the hello admission
// and through the fallback join_req. "other", an unknown or a malformed hint
// keeps the ID; a malformed route admits the member with none.
static void TestAdmissionRoutes() {
	const auto id = Route(1);
	struct Case { json latency; std::string endpoint, relay; };
	const std::vector<Case> cases = {
		{RouteJson(id), id, ""},
		{RouteJson(id, "other"), id, ""},
		{RouteJson(id, "xyz9"), id, ""},
		{json::object({{"endpoint", id}, {"relay", 5}}), id, ""},
		{RouteJson(id, "use1"), id, "use1"},
		{json("not a route"), "", ""},
		{json::array({id}), "", ""},
		{json::object({{"endpoint", 7}}), "", ""},
		{RouteJson(id.substr(1)), "", ""},
		{RouteJson(std::string(64, 'C')), "", ""},
		{json::object({{"relay", "use1"}}), "", ""},
		{json(nullptr), "", ""},
	};
	for (const bool hello : {true, false}) {
		for (const auto& item : cases) {
			LatencyRoom room;
			room.HelloJoin(1, json());
			if (hello) room.HelloJoin(2, item.latency);
			else room.FallbackJoin(2, item.latency);
			CHECK(room.server.roomMembers.count(2) == 1);
			const auto member = room.Member(2);
			const auto checkpoint = room.server.RecoveryCheckpoint();
			const auto& latency = ActiveRow(checkpoint, member).at("data").at("latency");
			if (item.endpoint.empty()) CHECK(latency == json::object());
			else CHECK(latency.at("endpoint") == item.endpoint);
			if (item.relay.empty()) CHECK(!latency.contains("relay"));
			else CHECK(latency.at("relay") == item.relay);
			CHECK(room.server.roomMemberRecords.at(member).latency.endpoint == item.endpoint);
			CHECK(room.server.roomMemberRecords.at(member).latency.relay == item.relay);
			// The client row is the record's projection.
			const auto legacy = room.server.Checkpoint();
			for (const auto& row : legacy.at("members"))
				if (row.at("connection") == 2) CHECK(row.at("data").at("latency") == latency);
			// A member whose join carried no route has none.
			CHECK(ActiveRow(checkpoint, room.Member(1)).at("data").at("latency") == json::object());
		}
	}
}

// C++ 2, 3 and 5: the legacy checkpoint into a fresh server and into one that
// holds other members, and a refused restore that changes nothing.
static void TestCanonicalRestore() {
	ThreeMembers source;
	CheckIdentities(source.room.server, source.expected);
	const auto checkpoint = json::parse(source.room.server.Checkpoint().dump());

	auto fresh = Fresh();
	CHECK(fresh->RestoreCheckpoint(checkpoint));
	CHECK(fresh->Checkpoint() == checkpoint);
	CheckIdentities(*fresh, source.expected);

	LatencyRoom other;
	other.incarnations = {{1, 3}, {2, 4}, {3, 5}, {4, 6}};
	for (session::Connection connection = 1; connection <= 4; ++connection)
		other.HelloJoin(connection, RouteJson(Route(10 + connection)));
	CHECK(other.server.roomMemberRecords.size() == 4);
	const auto otherCheckpoint = json::parse(other.server.Checkpoint().dump());
	CHECK(other.server.RestoreCheckpoint(checkpoint));
	CheckIdentities(other.server, source.expected);

	// Refused at its last step, after the members were read and normalized.
	const auto incarnations = fresh->roomIncarnations;
	const auto records = json(fresh->roomMemberRecords);
	const auto before = fresh->RecoveryCheckpoint();
	auto damaged = otherCheckpoint;
	damaged["match_authorities"] = nullptr;
	CHECK(!fresh->RestoreCheckpoint(damaged));
	CHECK(fresh->RestoreRefusal() == "match_authorities");
	CHECK(fresh->roomIncarnations == incarnations && json(fresh->roomMemberRecords) == records);
	CHECK(fresh->RecoveryCheckpoint() == before);
}

// C++ 4: rows and snapshot members from before these fields existed.
static void TestOldCheckpointJson() {
	ThreeMembers source;
	const auto checkpoint = json::parse(source.room.server.Checkpoint().dump());
	{
		auto old = checkpoint;
		for (auto& row : old.at("members")) row.at("data").erase("latency");
		auto restored = Fresh();
		CHECK(restored->RestoreCheckpoint(old));
		auto expected = source.expected;
		for (auto& item : expected) item.second.route.clear();
		CheckIdentities(*restored, expected);
	}
	auto old = checkpoint;
	const auto second = source.Member(2), third = source.Member(3);
	// No row incarnation: the snapshot member's.
	LegacyRow(old, second).at("data").erase("incarnation");
	SnapshotMember(old, second)["incarnation"] = std::uint64_t(23);
	// Neither: a snapshot member without one reads as 1.
	LegacyRow(old, third).at("data").erase("incarnation");
	SnapshotMember(old, third).erase("incarnation");
	auto restored = Fresh();
	CHECK(restored->RestoreCheckpoint(old));
	auto expected = source.expected;
	expected.at(second).incarnation = 23;
	expected.at(third).incarnation = 1;
	CheckIdentities(*restored, expected);
}

// C++ 6: portable restore and rebind keep the route with its incarnation, a
// rollback to the baseline too, and both incarnation fallbacks agree with the
// canonical restore.
static void TestPortableRestore() {
	ThreeMembers source;
	const auto portable = source.room.server.RecoveryCheckpoint();
	auto replica = Fresh();
	CHECK(replica->RestoreRecoveryCheckpoint(portable));
	CHECK(replica->RebindMembers(Bindings(portable, source.expected, 200)));
	CheckIdentities(*replica, source.expected);
	// A rebind takes the route from the record, whatever an older row held.
	for (auto& client : replica->clients) client.data.latency = LatencyRoute::Parsed(Route(99), "");
	CHECK(replica->RebindMembers(Bindings(portable, source.expected, 300)));
	CheckIdentities(*replica, source.expected);

	// Rollback: the baseline a candidate opens with is restored and rebound.
	MockTransport* transport = nullptr;
	auto rollback = Fresh(&transport);
	CHECK(rollback->RestoreRecoveryCheckpoint(portable));
	CHECK(rollback->RebindMembers(Bindings(portable, source.expected, 200)));
	rollback->SetAuthority(1, 0, true);
	const auto* snapshot = rollback->RoomSnapshot();
	protocol::RoomActionMessage queue;
	queue.action.kind = room::ActionKind::Queue; queue.action.roomEpoch = snapshot->roomEpoch;
	queue.action.revision = snapshot->revision; queue.action.tableRevision = snapshot->tables[0].revision; queue.action.actionId = 1;
	transport->Push(201, json(queue));
	CHECK(rollback->Step() == 0 && rollback->HasRecoveryCandidate());
	CHECK(rollback->RestoreRecoveryBaseline() && !rollback->HasRecoveryCandidate());
	CheckIdentities(*rollback, source.expected);

	{
		// Outer incarnation absent, data.incarnation 42.
		auto changed = portable;
		const auto third = source.Member(3);
		PortableRow(changed, third).erase("incarnation");
		CHECK(PortableRow(changed, third).at("data").at("incarnation") == 42);
		auto restored = Fresh();
		CHECK(restored->RestoreRecoveryCheckpoint(changed));
		CheckIdentities(*restored, source.expected);
		CHECK(restored->RebindMembers(Bindings(portable, source.expected, 200)));
		CheckIdentities(*restored, source.expected);
	}
	{
		// Both absent: the snapshot member's 17.
		auto changed = portable;
		const auto second = source.Member(2);
		PortableRow(changed, second).erase("incarnation");
		PortableRow(changed, second).at("data").erase("incarnation");
		SnapshotMember(changed, second)["incarnation"] = std::uint64_t(17);
		auto expected = source.expected;
		expected.at(second).incarnation = 17;
		auto restored = Fresh();
		CHECK(restored->RestoreRecoveryCheckpoint(changed));
		CheckIdentities(*restored, expected);
		CHECK(restored->RebindMembers(Bindings(portable, expected, 200)));
		CheckIdentities(*restored, expected);
	}
	{
		// A frozen row with neither field keeps 1 and leaves the active entries.
		auto changed = portable;
		changed.at("members").push_back({{"member", room::MemberId(99)}, {"endpoint", room::ConnectionRef{"latency-room", "gone"}},
			{"data", json::object({{"name", "Gone"}})}, {"frozen", true}});
		auto restored = Fresh();
		CHECK(restored->RestoreRecoveryCheckpoint(changed));
		CHECK(restored->roomFrozenMembers.at(99).incarnation == 1 && restored->roomFrozenMembers.at(99).data.incarnation == 1);
		CHECK(restored->roomIncarnations.at(99) == 1);
		CheckIdentities(*restored, source.expected, 1);
	}
}

// C++ 6 (frozen) and 7: a frozen row keeps its route through a portable
// restore. A dropped connection, Leave, a kick and Close drop the record, and
// an ID that a record, a frozen row or a peer identity holds is admitted as none.
static void TestRecordLifecycle() {
	LatencyRoom fixture;
	auto& server = fixture.server;
	const auto& records = server.roomMemberRecords;
	for (session::Connection connection = 1; connection <= 4; ++connection)
		fixture.HelloJoin(connection, RouteJson(Route(20 + connection)));
	fixture.Act(2, room::ActionKind::Queue); fixture.Act(3, room::ActionKind::Queue); fixture.Act(4, room::ActionKind::Watch);
	fixture.Act(2, room::ActionKind::Ready); fixture.Act(3, room::ActionKind::Ready);
	fixture.generation = server.RoomSnapshot()->tables[0].matchGeneration;
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	fixture.Acknowledge(2, "game_prepared", {{"spectators_optional", true}});
	fixture.Acknowledge(3, "game_prepared");
	// The spectator leaves while the generation still names it.
	const auto spectator = fixture.Member(4);
	fixture.Act(4, room::ActionKind::Leave);
	CHECK(server.roomMembers.count(4) == 0 && records.count(spectator) == 0);
	CHECK(server.roomFrozenMembers.count(spectator) == 1);
	CHECK(server.roomFrozenMembers.at(spectator).data.latency.endpoint == Route(24));
	const auto portable = server.RecoveryCheckpoint();
	auto restored = Fresh();
	CHECK(restored->RestoreRecoveryCheckpoint(portable));
	CHECK(restored->roomFrozenMembers.at(spectator).data.latency.endpoint == Route(24));
	CHECK(restored->roomMemberRecords.count(spectator) == 0 && restored->roomMemberRecords.size() == 3);

	fixture.HelloJoin(5, RouteJson(Route(24)));  // the frozen row's
	fixture.HelloJoin(6, RouteJson(Route(21)));  // member 1's record
	fixture.HelloJoin(7, RouteJson(Primary(2))); // member 2's peer identity
	fixture.HelloJoin(8, RouteJson(Route(28)));
	for (const session::Connection connection : {5, 6, 7}) {
		CHECK(server.roomMembers.count(connection) == 1);
		CHECK(!records.at(fixture.Member(connection)).latency.Valid());
		CHECK(ActiveRow(server.RecoveryCheckpoint(), fixture.Member(connection)).at("data").at("latency") == json::object());
	}
	CHECK(records.at(fixture.Member(1)).latency.endpoint == Route(21));
	CHECK(records.at(fixture.Member(8)).latency.endpoint == Route(28));

	const auto dropped = fixture.Member(8);
	fixture.transport->disconnected.push_back(8); fixture.Step();
	CHECK(server.roomMembers.count(8) == 0 && records.count(dropped) == 0 && server.roomIncarnations.count(dropped) == 0);
	const auto leaving = fixture.Member(7);
	fixture.Act(7, room::ActionKind::Leave);
	CHECK(server.roomMembers.count(7) == 0 && records.count(leaving) == 0);
	const auto kicked = fixture.Member(6);
	fixture.Act(1, room::ActionKind::Kick, kicked);
	CHECK(server.roomMembers.count(6) == 0 && records.count(kicked) == 0);
	// A route a departed member held can be admitted again.
	fixture.HelloJoin(9, RouteJson(Route(28)));
	CHECK(records.at(fixture.Member(9)).latency.endpoint == Route(28));
	// Every member has a record and an incarnation, and nothing else does.
	const auto& members = server.RoomSnapshot()->members;
	CHECK(records.size() == members.size() && server.roomIncarnations.size() == members.size());
	for (const auto& member : members) CHECK(records.count(member.id) == 1 && server.roomIncarnations.count(member.id) == 1);
	server.Close();
	CHECK(records.empty());
}

// C++ 8: one table of IDs against the shared predicate, IrohRoom's and the route.
static void TestEndpointIdentityPredicate() {
	struct Case { std::string text; bool valid; };
	const std::vector<Case> cases = {
		{Route(1), true}, {std::string(64, '0'), true}, {std::string(64, 'f'), true},
		{"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", true},
		{std::string(), false}, {std::string(63, 'a'), false}, {std::string(65, 'a'), false},
		{std::string(64, 'A'), false}, {std::string(63, 'a') + "G", false}, {std::string(63, 'a') + "g", false},
		{std::string(63, 'a') + " ", false}, {"0x" + std::string(62, 'a'), false},
		{std::string(63, 'a') + std::string(1, '\0'), false}, {std::string(32, 'a'), false},
	};
	for (const auto& item : cases) {
		CHECK(IsEndpointIdentity(item.text) == item.valid);
		CHECK(session::IrohRoom::IsEndpointIdentity(item.text) == item.valid);
		const LatencyRoute route{item.text, ""};
		CHECK(route.Valid() == item.valid);
		CHECK(LatencyRoute::Parsed(item.text, "use1").Valid() == item.valid);
	}
	// The JSON forms: {} for none, no relay key without a hint.
	CHECK(json(LatencyRoute{}) == json::object());
	CHECK(json(LatencyRoute::Parsed(Route(1), "other")) == RouteJson(Route(1)));
	CHECK(json(LatencyRoute::Parsed(Route(1), "euc1")) == RouteJson(Route(1), "euc1"));
	protocol::SessionJoinRequest request;
	CHECK(!json(request).contains("latency"));
	request.latency = LatencyRoute::Parsed(Route(1), "");
	CHECK(json(request).at("latency") == RouteJson(Route(1)));
	CHECK(json(request).get<protocol::SessionJoinRequest>().latency.endpoint == Route(1));
	CHECK(json(protocol::MemberData{}).at("latency") == json::object());
	json older = protocol::MemberData{};
	older.erase("latency");
	CHECK(!older.get<protocol::MemberData>().latency.Valid());
	older["latency"] = "x";
	CHECK(!older.get<protocol::MemberData>().latency.Valid());
}

// C++ 11: the room's own route comes from its hosted or connected event and is
// gone after a new Join or Host (a new epoch) or a change of room.
static void TestLocalLatencyRoute() {
	FakeHelperPipe pipe;
	platform::HelperClient helper;
	pipe.Connect(helper);
	auto room = std::make_shared<session::IrohRoom>(helper);
	const auto settle = [&](const auto& done) {
		for (int tick = 0; tick < 5000 && !done(); ++tick) { room->Poll(); pipe.Drain(); Sleep(1); }
		CHECK(done());
	};
	const std::array<std::uint8_t, 16> first{{1}}, second{{2}}, third{{3}};
	const auto connected = [&](const std::string& peer, const std::array<std::uint8_t, 16>& id, std::uint64_t control) {
		return json{{"type", "connected"}, {"transport", "online"}, {"epoch", room->Epoch()}, {"peer", peer}, {"room", id}, {"control", control}};
	};
	const auto settlePeer = [&](const std::string& peer) { settle([&] { return room->ConnectionForIdentity(peer) != 0; }); };

	CHECK(room->Host("build",sf4e::netplay::RoomKind::Online) && !room->LocalLatency().Valid());
	pipe.Send({{"type", "status"}, {"request_id", 1}, {"endpoint", Hex64('a', 1)}, {"fixed_port", 0}});
	pipe.Send({{"type", "hosted"}, {"transport", "online"}, {"epoch", room->Epoch()}, {"room", first}, {"invitation", "first"},
		{"latency", Route(1)}, {"latency_relay", "use1"}});
	settle([&] { return room->Invitation() == "first"; });
	CHECK(room->LocalLatency().endpoint == Route(1) && room->LocalLatency().relay == "use1");
	auto hostClient = room->Client();
	CHECK(hostClient && hostClient->LocalLatency().endpoint == Route(1) && hostClient->LocalLatency().relay == "use1");
	// A joiner's connected event, without the fields, leaves the host's route.
	pipe.Send(connected(Hex64('b', 1), first, 1));
	settlePeer(Hex64('b', 1));
	CHECK(room->LocalLatency().endpoint == Route(1));

	// A new Join is a new epoch.
	const auto hostEpoch = room->Epoch();
	room->Leave();
	pipe.Send({{"type", "room_closed"}, {"epoch", hostEpoch}});
	settle([&] { return room->GetState() == session::IrohRoom::State::Idle; });
	CHECK(room->Join("invitation", "build") && room->Epoch() == hostEpoch + 1);
	CHECK(!room->LocalLatency().Valid() && !hostClient->LocalLatency().Valid());
	auto event = connected(Hex64('d', 1), second, 1);
	event["latency"] = Route(2); event["latency_relay"] = "other";
	pipe.Send(event);
	settlePeer(Hex64('d', 1));
	CHECK(room->LocalLatency().endpoint == Route(2) && room->LocalLatency().relay.empty());
	auto joinClient = room->Client();
	CHECK(joinClient && joinClient->LocalLatency().endpoint == Route(2));
	// A present but malformed field is no route.
	event = connected(Hex64('d', 2), second, 2); event["latency"] = 12;
	pipe.Send(event);
	settlePeer(Hex64('d', 2));
	CHECK(!room->LocalLatency().Valid());
	event = connected(Hex64('d', 3), second, 3); event["latency"] = Route(2);
	pipe.Send(event);
	settlePeer(Hex64('d', 3));
	CHECK(room->LocalLatency().endpoint == Route(2));
	// Another room drops it.
	pipe.Send(connected(Hex64('d', 4), third, 4));
	settlePeer(Hex64('d', 4));
	CHECK(!room->LocalLatency().Valid() && !joinClient->LocalLatency().Valid());

	// So does a new Host, and a hosted event without the fields sets none.
	const auto joinEpoch = room->Epoch();
	room->Leave();
	pipe.Send({{"type", "room_closed"}, {"epoch", joinEpoch}});
	settle([&] { return room->GetState() == session::IrohRoom::State::Idle; });
	CHECK(room->Host("build",sf4e::netplay::RoomKind::Online) && room->Epoch() == joinEpoch + 1 && !room->LocalLatency().Valid());
	pipe.Send({{"type", "hosted"}, {"transport", "online"}, {"epoch", room->Epoch()}, {"room", first}, {"invitation", "again"}});
	settle([&] { return room->Invitation() == "again"; });
	CHECK(!room->LocalLatency().Valid());
}

int main() {
	TestEndpointIdentityPredicate();
	TestAdmissionRoutes();
	TestCanonicalRestore();
	TestOldCheckpointJson();
	TestPortableRestore();
	TestRecordLifecycle();
	TestLocalLatencyRoute();
	std::cout << "Session server latency tests passed\n";
	return 0;
}
