#include "../session/sf4e__SessionServer.hxx"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>

#include "test_support.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

class MockTransport final : public session::ServerTransport {
public:
	std::vector<session::Message> incoming;
	std::vector<session::Connection> disconnected;
	std::vector<std::pair<session::Connection, json>> outgoing;
	bool writable = true;
	bool closed = false;
	bool Listen(std::uint16_t) override { return true; }
	bool Attach(session::Connection) override { return true; }
	bool Poll(std::vector<session::Message>& messages,
		std::vector<session::Connection>& departed, std::size_t maximum) override {
		CHECK(maximum > 0);
		const auto count = (std::min)(incoming.size(), maximum);
		messages.insert(messages.end(), std::make_move_iterator(incoming.begin()),
			std::make_move_iterator(incoming.begin() + count));
		incoming.erase(incoming.begin(), incoming.begin() + count);
		departed.swap(disconnected);
		return !closed;
	}
	bool PollRecoveryPrefix(std::vector<session::Message>& messages,
		std::vector<session::Connection>& departed, std::size_t maximum,
		const BatchPredicate& batchable) override {
		CHECK(maximum > 0);
		std::size_t count = incoming.empty() ? 0 : 1;
		if (count && batchable(incoming.front()))
			while (count < incoming.size() && count < maximum && batchable(incoming[count])) ++count;
		messages.insert(messages.end(), std::make_move_iterator(incoming.begin()),
			std::make_move_iterator(incoming.begin() + count));
		incoming.erase(incoming.begin(), incoming.begin() + count);
		departed.swap(disconnected);
		return !closed;
	}
	bool Send(session::Connection connection, const std::string& payload) override {
		if (!writable) return false;
		outgoing.emplace_back(connection, json::parse(payload));
		return true;
	}
	void Close() override { closed = true; }
	void Push(session::Connection connection, json payload, std::int64_t id = 2) {
		incoming.push_back({connection, id, payload.dump(), ""});
	}
	bool Contains(const char* type) const {
		for (const auto& sent : outgoing) if (sent.second.at("type") == type) return true;
		return false;
	}
};

static void TestAtomicMatchRebind() {
	std::array<std::uint8_t, 16> room = {};
	room[0] = 7;
	session::MatchAuthority authority(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	std::vector<session::MatchAuthority::Participant> participants = {
		{1, {"relay", "member-a"}}, {2, {"relay", "member-b"}}
	};
	CHECK(authority.BeginAtGeneration(participants, 12, [](session::Connection, const json&) { return true; }));
	CHECK(authority.Acknowledge(1, json{{"type", "game_prepared"}, {"generation", 12}}, [](session::Connection, const json&) { return true; }));
	CHECK(authority.Acknowledge(2, json{{"type", "game_prepared"}, {"generation", 12}}, [](session::Connection, const json&) { return true; }));
	CHECK(authority.Acknowledge(1, json{{"type", "game_ready"}, {"generation", 12}}, [](session::Connection, const json&) { return true; }));
	CHECK(authority.Acknowledge(2, json{{"type", "game_ready"}, {"generation", 12}}, [](session::Connection, const json&) { return true; }));
	CHECK(authority.GetPhase() == session::MatchAuthority::Phase::Started);
	const auto digest = authority.CapabilityDigest();
	const auto swapped = std::vector<session::MatchAuthority::RebindEntry>{
		{{"relay", "member-a"}, 2}, {{"relay", "member-b"}, 1}};
	CHECK(authority.RebindConnections(swapped));
	CHECK(authority.CapabilityDigest() == digest);
	const auto rebound = authority.Checkpoint();
	for (const auto& row : rebound.at("participants")) {
		const auto endpoint = row.at("member").at("user").get<std::string>();
		if (endpoint == "member-a") CHECK(row.at("connection") == 2);
		if (endpoint == "member-b") CHECK(row.at("connection") == 1);
	}
	// The server passes the complete room roster to every table authority;
	// unrelated spectator endpoints must be accepted while only this authority's
	// participant subset is remapped.
	CHECK(authority.RebindConnections({{{"relay", "member-a"}, 1}, {{"relay", "member-b"}, 2}, {{"relay", "spectator"}, 3}}));
	CHECK(authority.CapabilityDigest() == digest);
	const auto beforeInvalid = authority.Checkpoint();
	CHECK(!authority.RebindConnections({{{"relay", "member-a"}, 1}, {{"relay", "member-b"}, 1}}));
	CHECK(authority.Checkpoint() == beforeInvalid);
}

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
	}
}

// Room chat is sent to a roomChatDelta client only when its committed copy is
// stale; an older client always gets the full chat.
static void TestRoomChatDelta() {
	auto* transport = new MockTransport();
	SessionServer server("chat-delta", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> room = {};
	room[0] = 31;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Chat delta", 16, 48);
	std::uint64_t request = 1, revision = 0;
	constexpr std::uint64_t term = 7;
	server.SetAuthority(term, revision, true);
	const auto commit = [&]() {
		transport->outgoing.clear();
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		const auto proposal = server.PendingProposal();
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
	};
	const auto admit = [&](session::Connection connection, bool chatDelta) {
		protocol::SessionJoinRequest join;
		join.username = "Chat-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
		join.customRooms = true; join.roomProtocol = room::ProtocolVersion; join.roomChatDelta = chatDelta;
		protocol::SessionHelloMsg hello; hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commit();
	};
	std::uint64_t actionId = 0;
	const auto act = [&](session::Connection connection, room::ActionKind kind, const std::string& text = "") {
		const auto snapshot = *server.RoomSnapshot();
		room::Action value;
		value.kind = kind; value.roomEpoch = snapshot.roomEpoch; value.revision = snapshot.revision;
		value.tableRevision = snapshot.tables[0].revision; value.actionId = ++actionId; value.table = 0; value.text = text;
		protocol::RoomActionMessage message; message.action = value;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
		commit();
	};
	// The broadcast snapshot each client received in the last commit.
	const auto snapshotFor = [&](session::Connection connection) {
		for (const auto& sent : transport->outgoing)
			if (sent.first == connection && sent.second.value("type", std::string()) == "room_snapshot") return sent.second.at("snapshot");
		return json();
	};
	admit(1, true);
	admit(2, false);
	act(1, room::ActionKind::Chat, "hello");
	CHECK(snapshotFor(1).at("chat").size() == 1 && snapshotFor(2).at("chat").size() == 1);
	act(1, room::ActionKind::Queue);
	CHECK(snapshotFor(1).value("chat_unchanged", false) && !snapshotFor(1).contains("chat"));
	CHECK(!snapshotFor(2).contains("chat_unchanged") && snapshotFor(2).at("chat").size() == 1);
	act(2, room::ActionKind::Chat, "again");
	CHECK(snapshotFor(1).at("chat").size() == 2);
	act(2, room::ActionKind::Queue);
	CHECK(snapshotFor(1).value("chat_unchanged", false));
	// A departure prunes the leaver's lines, which counts as a change.
	act(2, room::ActionKind::Leave);
	CHECK(snapshotFor(1).at("chat").size() == 1);
}

static void TestSameTermProposalPause() {
	auto* transport = new MockTransport();
	SessionServer server("same-term-pause", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> room = {};
	room[0] = 29;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Same-term pause", 16, 47);
	std::uint64_t request = 1, revision = 0;
	constexpr std::uint64_t term = 7;
	server.SetAuthority(term, revision, true);
	auto commitCandidate = [&](session::Connection bootstrapRecipient = 0) {
		CHECK(transport->outgoing.empty());
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
		const auto proposal = server.PendingProposal();
		CHECK(proposal != nullptr);
		if (bootstrapRecipient) CHECK(std::any_of(proposal->effects.begin(), proposal->effects.end(),
			[&](const session::EffectEnvelope& effect) {
				return effect.recipient == bootstrapRecipient && effect.type == "room_snapshot";
			}));
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		transport->outgoing.clear();
		++request;
		++revision;
	};
	auto admit = [&](session::Connection connection) {
		protocol::SessionJoinRequest join;
		join.username = "Pause-" + std::to_string(connection);
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(31000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commitCandidate(connection);
	};
	auto action = [&](session::Connection connection, room::ActionKind kind, std::uint64_t actionId) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action value;
		value.kind = kind;
		value.roomEpoch = snapshot.roomEpoch;
		value.revision = snapshot.revision;
		value.tableRevision = snapshot.tables[0].revision;
		value.actionId = actionId;
		value.table = 0;
		protocol::RoomActionMessage message;
		message.action = value;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
	};
	for (session::Connection connection = 1; connection <= 4; ++connection) admit(connection);
	for (session::Connection connection = 1; connection <= 4; ++connection) {
		action(connection, room::ActionKind::Queue, 1);
		commitCandidate();
	}
	action(1, room::ActionKind::Ready, 2);
	commitCandidate();
	action(2, room::ActionKind::Ready, 2);
	CHECK(server.HasRecoveryCandidate());
	CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
	const auto proposal = server.PendingProposal();
	CHECK(proposal != nullptr);
	const auto privateGrantCount = std::count_if(proposal->effects.begin(), proposal->effects.end(), [](const auto& effect) {
		return effect.privatePayload && effect.type == "game_prepare";
	});
	CHECK(privateGrantCount == 4);

	// A same-term health pause can race the helper's durable commit watch. Keep
	// the exact local candidate and its private grants until the room is rebound;
	// importing the digest-only checkpoint would permanently lose those grants.
	server.SetAuthority(term, revision, false);
	CHECK(server.PendingProposal() != nullptr);
	CHECK(server.PendingProposal()->effects == proposal->effects);
	CHECK(!server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
	CHECK(transport->outgoing.empty());
	server.SetAuthority(term, revision, true);
	CHECK(server.PendingProposal() != nullptr);
	CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
	std::set<session::Connection> grantRecipients;
	for (const auto& sent : transport->outgoing) {
		if (sent.second.value("type", std::string()) != "game_prepare") continue;
		CHECK(sent.second.contains("_commit"));
		grantRecipients.insert(sent.first);
	}
	CHECK(grantRecipients == std::set<session::Connection>({1, 2, 3, 4}));
	const auto sentCount = transport->outgoing.size();
	CHECK(!server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
	CHECK(transport->outgoing.size() == sentCount);
}

static void TestMaximumRoomResultBurst() {
	auto* transport = new MockTransport();
	SessionServer server("maximum-result-burst", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> authorizationRoom = {};
	authorizationRoom[0] = 61;
	server.EnableMatchAuthorization(authorizationRoom, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Maximum result burst", room::MaximumMembers, 71);
	constexpr std::uint64_t term = 5;
	std::uint64_t request = 1, revision = 0;
	server.SetAuthority(term, revision, true);
	std::set<std::pair<session::Connection, std::uint64_t>> replies;
	std::size_t acceptedReplies = 0;
	std::size_t latestProposalBytes = 0, latestProposalEffects = 0;
	auto commitCandidate = [&](bool collectReplies) {
		CHECK(server.HasRecoveryCandidate());
		CHECK(!server.RecoveryCandidateOverflowed());
		CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
		const auto proposal = server.PendingProposal();
		CHECK(proposal != nullptr);
		latestProposalBytes = json(*proposal).dump().size();
		latestProposalEffects = proposal->effects.size();
		CHECK(session::EffectJournalBytes(proposal->effects) <= session::MaxEffectJournalBytes);
		CHECK(latestProposalBytes <= 1024 * 1024);
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		for (const auto& outgoing : transport->outgoing) {
			CHECK(outgoing.second.dump().size() <= 64 * 1024);
			if (collectReplies && outgoing.second.value("type", std::string()) == "room_result") {
				replies.emplace(outgoing.first, outgoing.second.value("actionId", std::uint64_t(0)));
				if (outgoing.second.at("result").at("accepted").get<bool>()) ++acceptedReplies;
			}
		}
		transport->outgoing.clear();
		++request; ++revision;
	};
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection) {
		protocol::SessionJoinRequest join;
		join.username = "Burst-" + std::to_string(connection);
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(32000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commitCandidate(false);
	}
	CHECK(server.RoomSnapshot()->members.size() == room::MaximumMembers);

	// Fill the retained public projection to its validated chat bound. Quotes
	// and backslashes exercise JSON expansion; the first ordinary message also
	// proves that player text containing "capability" remains replayable.
	for (std::uint64_t actionId = 1; actionId <= room::MaximumChatMessages; ++actionId) {
		server.AdvanceCustomRoom(actionId * 1000);
		CHECK(!server.HasRecoveryCandidate());
		const auto snapshot = *server.RoomSnapshot();
		room::Action action;
		action.kind = room::ActionKind::Chat;
		action.roomEpoch = snapshot.roomEpoch;
		action.revision = snapshot.revision;
		action.tableRevision = snapshot.tables[0].revision;
		action.actionId = actionId;
		if (actionId == 1) action.text = std::string(room::MaximumChatBytes - 10, 'x') + "capability";
		else action.text = std::string(room::MaximumChatBytes, actionId & 1 ? '\\' : '"');
		protocol::RoomActionMessage message;
		message.action = action;
		transport->Push(1, json(message));
		CHECK(server.Step() == 0);
		commitCandidate(false);
	}
	CHECK(server.RoomSnapshot()->chat.size() == room::MaximumChatMessages);
	CHECK(std::none_of(server.CommittedEffectHistory().begin(), server.CommittedEffectHistory().end(),
		[](const session::EffectEnvelope& effect) { return effect.type == "room_snapshot" && effect.privatePayload; }));

	// The first queued intent mutates the full room. Fifteen further members
	// submit explicit stale-room actions at the same time. Recovery polls one
	// intent per candidate, so every large result is committed and delivered
	// without overflowing or losing the remaining bounded transport queue.
	server.AdvanceCustomRoom((room::MaximumChatMessages + 1) * 1000);
	CHECK(!server.HasRecoveryCandidate());
	const auto beforeBurst = *server.RoomSnapshot();
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection) {
		room::Action action;
		action.kind = room::ActionKind::Chat;
		action.roomEpoch = connection == 1 ? beforeBurst.roomEpoch : beforeBurst.roomEpoch + 1;
		action.revision = beforeBurst.revision;
		action.tableRevision = beforeBurst.tables[0].revision;
		action.actionId = connection == 1 ? room::MaximumChatMessages + 1 : 1;
		action.text = std::string(room::MaximumChatBytes, connection & 1 ? '\\' : '"');
		protocol::RoomActionMessage message;
		message.action = action;
		transport->Push(connection, json(message));
	}
	for (std::size_t remaining = room::MaximumMembers; remaining > 0; --remaining) {
		CHECK(transport->incoming.size() == remaining);
		CHECK(server.Step() == 0);
		CHECK(transport->incoming.size() == remaining - 1);
		commitCandidate(true);
		if (remaining == room::MaximumMembers) {
			CHECK(latestProposalEffects == room::MaximumMembers * 2 + 1);
			std::cout << "Maximum retained-room single-intent proposal bytes=" << latestProposalBytes
				<< " effects=" << latestProposalEffects << '\n';
		}
	}
	CHECK(replies.size() == room::MaximumMembers);
	CHECK(acceptedReplies == 1);
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection)
		CHECK(replies.count({connection, connection == 1 ? room::MaximumChatMessages + 1 : 1}) == 1);
}

// A recipient that missed its MatchEnded (lost while reconnecting) can never
// acknowledge the receipt, and the table stays fenced. The server must be
// able to replay exactly the receipts a member still owes: once, to the
// member's connection, with the terminal-replay flag, plus the native
// game_end the fighter may still be waiting for.
static void TestTerminalReceiptReplay() {
	auto* transport = new MockTransport();
	SessionServer server("terminal-replay", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> authorizationRoom = {};
	authorizationRoom[0] = 74;
	server.EnableMatchAuthorization(authorizationRoom, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Terminal replay", 4, 80);
	CHECK(server.Listen(0) == 0);
	for (session::Connection connection = 1; connection <= 3; ++connection) {
		protocol::SessionJoinRequest join;
		join.username = "Replay-" + std::to_string(connection);
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(34000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		CHECK(server.roomMembers.count(connection) == 1);
	}
	const auto admitted = *server.RoomSnapshot();
	room::RoomAuthority model("Terminal replay", 4, admitted.roomEpoch);
	for (const auto& member : admitted.members) {
		CHECK(model.Join(member.name, member.connection, member.id == admitted.host, member.mainFighter).accepted);
		model.SetMemberIncarnation(member.id, member.incarnation);
	}
	const auto applyTable = [&](room::MemberId member, room::ActionKind kind, std::uint64_t actionId) {
		room::Action action;
		action.kind = kind;
		action.roomEpoch = model.SnapshotView().roomEpoch;
		action.revision = model.SnapshotView().revision;
		action.tableRevision = model.SnapshotView().tables[0].revision;
		action.actionId = actionId;
		action.table = 0;
		return model.Apply(member, action);
	};
	CHECK(applyTable(admitted.members[0].id, room::ActionKind::Queue, 1).accepted);
	CHECK(applyTable(admitted.members[1].id, room::ActionKind::Queue, 2).accepted);
	CHECK(applyTable(admitted.members[2].id, room::ActionKind::Watch, 3).accepted);
	const auto seated = model.SnapshotView().tables[0];
	CHECK(applyTable(seated.p1, room::ActionKind::Ready, 4).accepted);
	CHECK(applyTable(seated.p2, room::ActionKind::Ready, 5).accepted);
	CHECK(model.BeginMatch(0, seated.p1, seated.p2).accepted);
	const auto generation = model.SnapshotView().tables[0].matchGeneration;
	CHECK(model.EndMatch(0, generation, room::MatchResult::P2Win).accepted);
	// The spectator already acknowledged; the two fighters did not.
	{
		room::Action ack;
		ack.kind = room::ActionKind::AcknowledgeTerminal;
		ack.roomEpoch = model.SnapshotView().roomEpoch;
		ack.table = 0; ack.matchGeneration = generation; ack.actionId = 6;
		CHECK(model.Apply(admitted.members[2].id, ack).accepted);
	}
	auto checkpoint = server.Checkpoint();
	checkpoint["room"] = model.Checkpoint();
	CHECK(server.RestoreCheckpoint(checkpoint));

	const auto typeNamed = [](const json& message, const char* type) {
		return message.contains("type") && message.at("type").is_string() && message.at("type").get<std::string>() == type;
	};
	const auto replayedEvents = [&](session::Connection connection) {
		std::size_t events = 0;
		for (const auto& sent : transport->outgoing) {
			if (sent.first != connection) continue;
			if (!typeNamed(sent.second, "room_event")) continue;
			const auto event = sent.second.at("event");
			CHECK(event.at("kind").get<int>() == static_cast<int>(room::Event::Kind::MatchEnded));
			CHECK(event.at("match_generation").get<std::uint64_t>() == generation);
			CHECK(event.at("result").get<int>() == static_cast<int>(room::MatchResult::P2Win));
			CHECK(event.at("terminal_replay").get<bool>());
			++events;
		}
		return events;
	};
	const auto nativeEnds = [&](session::Connection connection) {
		std::size_t count = 0;
		for (const auto& sent : transport->outgoing)
			if (sent.first == connection && typeNamed(sent.second, "game_end") &&
				sent.second.at("generation").get<std::uint64_t>() == generation) ++count;
		return count;
	};
	// A fighter that owes the receipt gets exactly one replay and one game_end.
	transport->outgoing.clear();
	CHECK(server.ReplayPendingTerminalEvents(1, server.roomMembers.at(1)) == 1);
	CHECK(replayedEvents(1) == 1);
	// game_end is an after-data message: it leaves with the next step.
	CHECK(server.Step() == 0);
	CHECK(replayedEvents(1) == 1);
	CHECK(nativeEnds(1) == 1);
	CHECK(replayedEvents(2) == 0);
	// The spectator that already acknowledged owes nothing.
	transport->outgoing.clear();
	CHECK(server.ReplayPendingTerminalEvents(3, server.roomMembers.at(3)) == 0);
	CHECK(transport->outgoing.empty());
	// Unknown member or connection: nothing is sent.
	CHECK(server.ReplayPendingTerminalEvents(0, server.roomMembers.at(2)) == 0);
	CHECK(server.ReplayPendingTerminalEvents(2, 0) == 0);
	CHECK(transport->outgoing.empty());
	// An acknowledgement for a generation the ledger does not hold is
	// answered with the receipts the member actually owes.
	{
		room::Action wrong;
		wrong.kind = room::ActionKind::AcknowledgeTerminal;
		wrong.roomEpoch = server.RoomSnapshot()->roomEpoch;
		wrong.table = 0; wrong.matchGeneration = generation + 40; wrong.actionId = 7;
		protocol::RoomActionMessage message;
		message.action = wrong;
		transport->outgoing.clear();
		transport->Push(2, json(message));
		CHECK(server.Step() == 0);
		bool rejected = false;
		for (const auto& sent : transport->outgoing)
			if (sent.first == 2 && typeNamed(sent.second, "room_result"))
				rejected = !sent.second.at("result").at("accepted").get<bool>();
		CHECK(rejected);
		CHECK(replayedEvents(2) == 1);
		CHECK(nativeEnds(2) == 1);
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

static void TestTerminalAcknowledgmentBatch() {
	auto* transport = new MockTransport();
	SessionServer server("terminal-ack-batch", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> authorizationRoom = {};
	authorizationRoom[0] = 73;
	server.EnableMatchAuthorization(authorizationRoom, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Terminal ACK batch", room::MaximumMembers, 79);
	std::uint64_t request = 1, revision = 0;
	constexpr std::uint64_t term = 7;
	server.SetAuthority(term, revision, true);
	auto commitCandidate = [&]() {
		CHECK(server.HasRecoveryCandidate());
		CHECK(!server.RecoveryCandidateOverflowed());
		CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
		const auto proposal = server.PendingProposal();
		CHECK(proposal != nullptr);
		CHECK(json(*proposal).dump().size() <= 1024 * 1024);
		CHECK(session::EffectJournalBytes(proposal->effects) <= session::MaxEffectJournalBytes);
		CHECK(transport->outgoing.empty());
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
		return proposal;
	};
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection) {
		protocol::SessionJoinRequest join;
		join.username = "ACK-" + std::to_string(connection);
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(33000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commitCandidate();
		transport->outgoing.clear();
	}

	// Build a validated maximum room projection with one terminal receipt for
	// every authenticated member. Injecting that portable RoomAuthority state
	// avoids conflating this transport batching test with native preparation.
	const auto admitted = *server.RoomSnapshot();
	room::RoomAuthority model("Terminal ACK batch", room::MaximumMembers, admitted.roomEpoch);
	for (const auto& member : admitted.members) {
		CHECK(model.Join(member.name, member.connection, member.id == admitted.host, member.mainFighter).accepted);
		CHECK(model.SnapshotView().members.back().id == member.id);
		model.SetMemberIncarnation(member.id, member.incarnation);
	}
	for (std::uint64_t index = 1; index <= room::MaximumChatMessages; ++index) {
		model.AdvanceTime(index * 1000);
		room::Action chat;
		chat.kind = room::ActionKind::Chat;
		chat.roomEpoch = model.SnapshotView().roomEpoch;
		chat.revision = model.SnapshotView().revision;
		chat.actionId = index;
		chat.text = std::string(room::MaximumChatBytes, index & 1 ? '\\' : '"');
		CHECK(model.Apply(admitted.host, chat).accepted);
	}
	const auto applyTable = [&](room::MemberId member, room::ActionKind kind, std::uint64_t actionId) {
		room::Action action;
		action.kind = kind;
		action.roomEpoch = model.SnapshotView().roomEpoch;
		action.revision = model.SnapshotView().revision;
		action.tableRevision = model.SnapshotView().tables[0].revision;
		action.actionId = actionId;
		action.table = 0;
		return model.Apply(member, action);
	};
	CHECK(applyTable(admitted.members[0].id, room::ActionKind::Queue, 1001).accepted);
	CHECK(applyTable(admitted.members[1].id, room::ActionKind::Queue, 1002).accepted);
	for (std::size_t index = 2; index < admitted.members.size(); ++index)
		CHECK(applyTable(admitted.members[index].id, room::ActionKind::Watch, 1001 + index).accepted);
	const auto seated = model.SnapshotView().tables[0];
	CHECK(applyTable(seated.p1, room::ActionKind::Ready, 2001).accepted);
	CHECK(applyTable(seated.p2, room::ActionKind::Ready, 2002).accepted);
	CHECK(model.BeginMatch(0, seated.p1, seated.p2).accepted);
	const auto generation = model.SnapshotView().tables[0].matchGeneration;
	CHECK(model.EndMatch(0, generation, room::MatchResult::P1Win).accepted);
	CHECK(model.TerminalMembers(0, generation).size() == room::MaximumMembers);
	auto checkpoint = server.Checkpoint();
	checkpoint["room"] = model.Checkpoint();
	CHECK(server.RestoreCheckpoint(checkpoint));
	CHECK(server.RoomSnapshot()->chat.size() == room::MaximumChatMessages);
	server.AdvanceCustomRoom((room::MaximumChatMessages + 1) * 1000);
	CHECK(!server.HasRecoveryCandidate());

	// Matching fighter reports are durable evidence, but each report need not
	// pay for a separate full-state Raft commit. Consume the ordered result
	// prefix together and leave the following ordinary action at the queue head.
	const auto terminalSnapshot = *server.RoomSnapshot();
	for (std::size_t index = 0; index < 2; ++index) {
		room::Action report;
		report.kind = room::ActionKind::RecordResult;
		report.roomEpoch = terminalSnapshot.roomEpoch;
		report.actionId = 8000 + index;
		report.table = 0;
		report.matchGeneration = generation;
		report.result = room::MatchResult::P1Win;
		protocol::RoomActionMessage message;
		message.action = report;
		transport->Push(index + 1, json(message));
	}
	room::Action reportBoundary;
	reportBoundary.kind = room::ActionKind::SetCapacity;
	reportBoundary.roomEpoch = terminalSnapshot.roomEpoch;
	reportBoundary.revision = terminalSnapshot.revision;
	reportBoundary.actionId = 8999;
	reportBoundary.capacity = room::MaximumMembers;
	protocol::RoomActionMessage reportBoundaryMessage;
	reportBoundaryMessage.action = reportBoundary;
	transport->Push(1, json(reportBoundaryMessage));
	CHECK(server.Step() == 0);
	CHECK(transport->incoming.size() == 1);
	const auto reportProposal = commitCandidate();
	CHECK(std::count_if(reportProposal->effects.begin(), reportProposal->effects.end(), [](const auto& effect) {
		return effect.type == "room_result";
	}) == 2);
	transport->outgoing.clear();
	CHECK(server.Step() == 0);
	CHECK(transport->incoming.empty());
	commitCandidate();
	transport->outgoing.clear();

	std::vector<json> acknowledgments;
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action action;
		action.kind = room::ActionKind::AcknowledgeTerminal;
		action.roomEpoch = snapshot.roomEpoch;
		action.revision = snapshot.revision;
		action.tableRevision = snapshot.tables[0].revision;
		action.actionId = 9000 + connection;
		action.table = 0;
		action.matchGeneration = generation;
		protocol::RoomActionMessage message;
		message.action = action;
		acknowledgments.push_back(json(message));
		transport->Push(connection, acknowledgments.back());
	}
	const auto beforeBatch = *server.RoomSnapshot();
	room::Action followingChat;
	followingChat.kind = room::ActionKind::Chat;
	followingChat.roomEpoch = beforeBatch.roomEpoch;
	followingChat.revision = beforeBatch.revision;
	followingChat.actionId = 10000;
	followingChat.text = std::string(room::MaximumChatBytes, '\\');
	protocol::RoomActionMessage followingMessage;
	followingMessage.action = followingChat;
	transport->Push(1, json(followingMessage));
	CHECK(transport->incoming.size() == room::MaximumMembers + 1);
	CHECK(server.Step() == 0);
	// A recovery batch is an ordered terminal-ACK prefix. The following ordinary
	// action remains transport-owned until the ACK candidate commits.
	CHECK(transport->incoming.size() == 1);
	CHECK(server.HasRecoveryCandidate());
	CHECK(!server.RecoveryCandidateOverflowed());
	CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
	const auto acknowledgmentProposal = server.PendingProposal();
	CHECK(acknowledgmentProposal != nullptr);
	CHECK(json(*acknowledgmentProposal).dump().size() <= 1024 * 1024);
	CHECK(session::EffectJournalBytes(acknowledgmentProposal->effects) <= session::MaxEffectJournalBytes);
	CHECK(std::count_if(acknowledgmentProposal->effects.begin(), acknowledgmentProposal->effects.end(), [](const auto& effect) {
		return effect.type == "room_result" && effect.publicPayload.is_null() && !effect.privatePayload;
	}) == room::MaximumMembers);
	CHECK(std::count_if(acknowledgmentProposal->effects.begin(), acknowledgmentProposal->effects.end(), [](const auto& effect) {
		return effect.type == "room_snapshot";
	}) == room::MaximumMembers);
	CHECK(std::count_if(acknowledgmentProposal->effects.begin(), acknowledgmentProposal->effects.end(), [](const auto& effect) {
		return effect.type == "data_update";
	}) == room::MaximumMembers);
	CHECK(transport->outgoing.empty());
	CHECK(server.ApplyCommit(request, term, revision + 1, acknowledgmentProposal->checkpoint,
		acknowledgmentProposal->effectsDigest));
	++request; ++revision;
	CHECK(std::count_if(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& message) {
		return message.second.value("type", std::string()) == "room_result" &&
			message.second.at("result").at("accepted").get<bool>();
	}) == room::MaximumMembers);
	CHECK(!server.RoomSnapshot()->terminalPending[0]);
	transport->outgoing.clear();

	// The non-ACK behind the prefix is processed by the next one-intent
	// candidate and retains the ordinary public reply/replay behavior.
	CHECK(server.Step() == 0);
	CHECK(transport->incoming.empty());
	const auto chatProposal = commitCandidate();
	CHECK(std::any_of(chatProposal->effects.begin(), chatProposal->effects.end(), [](const auto& effect) {
		return effect.type == "room_result" && !effect.publicPayload.is_null();
	}));
	CHECK(std::any_of(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& message) {
		return message.second.value("type", std::string()) == "room_result" &&
			message.second.value("actionId", std::uint64_t(0)) == 10000 &&
			message.second.at("result").at("accepted").get<bool>();
	}));
	transport->outgoing.clear();

	// Syntactically valid but unauthorized generations receive the same bounded,
	// commit-gated digest envelopes; authority checks still reject every one.
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection) {
		auto invalid = acknowledgments[connection - 1];
		invalid["action"]["action_id"] = 12000 + connection;
		invalid["action"]["match_generation"] = generation + 100;
		transport->Push(connection, invalid);
	}
	CHECK(server.Step() == 0);
	CHECK(transport->incoming.empty());
	CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
	const auto rejectedProposal = server.PendingProposal();
	CHECK(rejectedProposal != nullptr && rejectedProposal->effects.size() == room::MaximumMembers);
	CHECK(json(*rejectedProposal).dump().size() <= 1024 * 1024);
	CHECK(std::all_of(rejectedProposal->effects.begin(), rejectedProposal->effects.end(), [](const auto& effect) {
		return effect.type == "room_result" && effect.publicPayload.is_null() && !effect.privatePayload;
	}));
	CHECK(transport->outgoing.empty());
	CHECK(server.ApplyCommit(request, term, revision + 1, rejectedProposal->checkpoint, rejectedProposal->effectsDigest));
	++request; ++revision;
	CHECK(std::count_if(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& message) {
		return message.second.value("type", std::string()) == "room_result" &&
			!message.second.at("result").at("accepted").get<bool>() &&
			message.second.at("result").at("reason").get<int>() == static_cast<int>(room::RejectReason::WrongGeneration);
	}) == room::MaximumMembers);

	// Simulate successor import after the ACK state commits but before any local
	// reply is delivered. Digest-only ACK replies are recovered by the client's
	// stable exact retry against the imported receipt/tombstone.
	auto imported = acknowledgmentProposal->checkpoint;
	auto history = imported.value("effect_journal", std::vector<session::EffectEnvelope>{});
	history.insert(history.end(), acknowledgmentProposal->effects.begin(), acknowledgmentProposal->effects.end());
	session::CompactEffectJournal(history);
	imported["effect_journal"] = history;
	imported["authority"] = {{"term", term}, {"revision", acknowledgmentProposal->baseRevision + 1}, {"writable", false}};
	auto* replicaTransport = new MockTransport();
	SessionServer replica("terminal-ack-batch", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(replicaTransport));
	replica.EnableMatchAuthorization(authorizationRoom, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	CHECK(replica.RestoreRecoveryCheckpoint(imported));
	{
		// The recovery bridge imports the leader's untouched checkpoint with the
		// compacted journal and authority passed beside it. That must produce
		// the same replica as splicing both into a JSON copy first.
		SessionServer direct("terminal-ack-batch", "build", true, 3, {0, 99},
			std::unique_ptr<session::ServerTransport>(new MockTransport()));
		direct.EnableMatchAuthorization(authorizationRoom, [](session::Connection) { return std::string(64, '0'); });
		CHECK(direct.RestoreRecoveryCheckpoint(acknowledgmentProposal->checkpoint, history,
			session::AuthorityStamp{term, acknowledgmentProposal->baseRevision + 1, false}));
		CHECK(direct.RecoveryCheckpoint() == replica.RecoveryCheckpoint());
		CHECK(direct.CommittedEffectHistory() == history);
	}
	std::vector<SessionServer::StableRebind> bindings;
	for (const auto& row : imported.at("members")) {
		const auto data = row.at("data").get<protocol::MemberData>();
		bindings.emplace_back(row.at("member").get<room::MemberId>(), data.roomMember,
			data.connId, row.at("incarnation").get<std::uint64_t>());
	}
	CHECK(replica.RebindMembers(bindings));
	replica.SetAuthority(term, acknowledgmentProposal->baseRevision + 1, true);
	replicaTransport->Push(1, acknowledgments[0]);
	CHECK(replica.Step() == 0);
	CHECK(replica.HasRecoveryCandidate() && !replica.RecoveryCandidateOverflowed());
	CHECK(replica.ProposeCheckpoint(1, term, acknowledgmentProposal->baseRevision + 1, replica.RecoveryCheckpoint()));
	const auto retryProposal = replica.PendingProposal();
	CHECK(retryProposal != nullptr && retryProposal->effects.size() == 1);
	CHECK(retryProposal->effects[0].type == "room_result" && retryProposal->effects[0].publicPayload.is_null());
	CHECK(replicaTransport->outgoing.empty());
	CHECK(replica.ApplyCommit(1, term, acknowledgmentProposal->baseRevision + 2,
		retryProposal->checkpoint, retryProposal->effectsDigest));
	CHECK(replicaTransport->outgoing.size() == 1);
	CHECK(replicaTransport->outgoing[0].second.at("result").at("accepted").get<bool>());
}

static void CheckIdleRecoveryWork(SessionServer& server, const char* phase) {
	server.AdvanceCustomRoom(0);
	CHECK(server.Step()==0 && !server.HasRecoveryCandidate());
	const auto before=server.RecoveryCheckpoint();
	const auto builds=server.RecoveryCheckpointBuilds();
	const auto started=std::chrono::steady_clock::now();
	for(int tick=0;tick<240;++tick) {
		server.AdvanceCustomRoom(0);
		CHECK(server.Step()==0 && !server.HasRecoveryCandidate());
	}
	const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
	const auto built=server.RecoveryCheckpointBuilds()-builds;
	std::cout << "Idle recovery " << phase << ": ticks=240 checkpoint_builds=" << built
		<< " total_ms=" << elapsed << " mean_ms=" << elapsed/240 << std::endl;
	CHECK(server.RecoveryCheckpoint()==before);
	CHECK(built==0);
}

static void TestCommittedSessionGate() {
	auto* transport = new MockTransport();
	SessionServer server("gated-room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> room = {};
	room[0] = 17;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Gated room", 16, 23);
	server.SetAuthority(1, 0, true);
	std::uint64_t request = 1, revision = 0, term = 1;
	auto commitCandidate = [&](bool expectOutput, const char* expectedType = nullptr, int expectedRoomAcceptance = -1,
		int expectedRoomReason = -1, int expectedRoomwide = -1, int expectedEffectCount = -1) {
		// Every gated effect is withheld until the exact proposal is accepted.
		CHECK(transport->outgoing.empty());
		CHECK(server.HasRecoveryCandidate());
		const auto candidate = server.RecoveryCheckpoint();
		CHECK(server.ProposeCheckpoint(request, term, revision, candidate));
		const auto proposal = server.PendingProposal();
		CHECK(proposal != nullptr);
		if (expectedRoomwide >= 0) {
			const auto hasSnapshot = std::any_of(proposal->effects.begin(), proposal->effects.end(), [](const auto& effect) {
				return effect.type == "room_snapshot";
			});
			const auto hasProjection = std::any_of(proposal->effects.begin(), proposal->effects.end(), [](const auto& effect) {
				return effect.type == "data_update";
			});
			if (hasSnapshot != (expectedRoomwide != 0) || hasProjection != (expectedRoomwide != 0)) {
				std::cerr << "Committed room action effects expected_roomwide=" << expectedRoomwide << " types=";
				for (const auto& effect : proposal->effects) std::cerr << effect.type << ',';
				std::cerr << '\n';
			}
			CHECK(hasSnapshot == (expectedRoomwide != 0));
			CHECK(hasProjection == (expectedRoomwide != 0));
		}
		if (expectedEffectCount >= 0) CHECK(proposal->effects.size() == static_cast<std::size_t>(expectedEffectCount));
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		if (expectOutput) {
			CHECK(!transport->outgoing.empty());
			if (expectedType) {
				CHECK(std::any_of(transport->outgoing.begin(), transport->outgoing.end(), [&](const auto& message) {
					return message.second.value("type", std::string()) == expectedType;
				}));
			}
			if (expectedRoomAcceptance >= 0) {
				const auto response = std::find_if(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& message) {
					return message.second.value("type", std::string()) == "room_result";
				});
				CHECK(response != transport->outgoing.end());
				CHECK(response->second.at("result").at("accepted").get<bool>() == (expectedRoomAcceptance != 0));
				if (expectedRoomReason >= 0)
					CHECK(response->second.at("result").at("reason").get<int>() == expectedRoomReason);
			}
		} else CHECK(transport->outgoing.empty());
		transport->outgoing.clear();
		++request;
		++revision;
	};
	auto admissionHello = [&](session::Connection connection, const char* name) {
		protocol::SessionJoinRequest join;
		join.username = name;
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(30000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		CHECK(server.cidMap.count(connection) == 1 && server.roomMembers.count(connection) == 1);
		commitCandidate(true, "hello_resp");
	};
	admissionHello(1, "Gate-one");
	admissionHello(2, "Gate-two");
	CheckIdleRecoveryWork(server,"two-member room");
	const auto action = [&](session::Connection connection, room::ActionKind kind, std::uint64_t actionId,
		std::uint8_t table = 0) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action value;
		value.kind = kind;
		value.roomEpoch = snapshot.roomEpoch;
		value.revision = snapshot.revision;
		value.tableRevision = snapshot.tables[table].revision;
		value.actionId = actionId;
		value.table = table;
		protocol::RoomActionMessage message;
		message.action = value;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
	};
	action(1, room::ActionKind::Queue, 1);
	commitCandidate(true, "room_result");
	action(2, room::ActionKind::Queue, 1);
	commitCandidate(true, "room_result");
	action(1, room::ActionKind::Ready, 2);
	commitCandidate(true, "room_result");

	// The second Ready creates native capabilities privately.  A stale term
	// must discard that preparation before it can reach the transport.  A
	// fighter report arriving while the timer candidate is still private must
	// be appended to that same candidate; Step must not starve it behind the
	// later quorum dispute.
	action(2, room::ActionKind::Ready, 2);
	const auto candidateBeforeReport = server.RecoveryCheckpoint();
	// This is the first native generation for the table in this fixture; the
	// room checkpoint keeps the table model portable but does not expose its
	// internal generation under a stable test-only JSON shape.
	const auto candidateGeneration = std::uint64_t(1);
	transport->Push(1, json{{"type", "game_prepared"}, {"generation", candidateGeneration}});
	CHECK(server.Step() == 0);
	CHECK(server.HasRecoveryCandidate());
	CHECK(server.RecoveryCheckpoint() != candidateBeforeReport);
	CHECK(transport->outgoing.empty());
	CHECK(server.HasRecoveryCandidate());
	CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
	CHECK(server.PendingProposal() != nullptr);
	// The bytes retained for the helper are exactly the proposal's encoding.
	CHECK(server.PendingProposal()->encoded == json(*server.PendingProposal()).dump());
	server.SetAuthority(2, revision, false);
	CHECK(server.PendingProposal() == nullptr);
	CHECK(transport->outgoing.empty());
	CHECK(server.RoomSnapshot()->tables[0].phase != room::TablePhase::Playing);
	for (const auto& effect : server.CommittedEffectHistory()) {
		if (effect.type == "game_prepare") CHECK(effect.publicPayload.is_null());
	}

	// The restored Ready state can be promoted by the new term. First commit a
	// native preparation, then simulate its leadership loss. The replacement
	// owner must journal game_end and the room transition before a new grant.
	term = 2;
	server.SetAuthority(term, revision, true);
	action(2, room::ActionKind::Ready, 3);
	commitCandidate(true, "game_prepare");
	const auto interruptedGeneration = server.RoomSnapshot()->tables[0].matchGeneration;
	term = 3;
	server.SetAuthority(term, revision, false);
	CHECK(transport->outgoing.empty());
	server.SetAuthority(term, revision, true);
	CHECK(server.CancelInterruptedPreparations());
	CHECK(transport->outgoing.empty());
	commitCandidate(true, "game_end");
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Waiting);
	auto terminalAction = [&](session::Connection connection, std::uint64_t actionId,
		std::uint64_t generation = 0, std::uint64_t epoch = 0) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action value;
		value.kind = room::ActionKind::AcknowledgeTerminal;
		value.roomEpoch = epoch ? epoch : snapshot.roomEpoch;
		value.revision = snapshot.revision;
		value.tableRevision = snapshot.tables[0].revision;
		value.actionId = actionId;
		value.table = 0;
		value.matchGeneration = generation ? generation : interruptedGeneration;
		protocol::RoomActionMessage message;
		message.action = value;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
	};
	// Recovery cancellation creates the same durable terminal obligation as a
	// played match. Both frozen native recipients must persist and acknowledge
	// that exact generation before either can start the next match.
	terminalAction(1, 3);
	commitCandidate(true, "room_result", 1, -1, 1);
	// The first ACK reply can disappear while unrelated allowed traffic advances
	// the member's generic action watermark. Its stable terminal identity must
	// remain idempotent without lowering that newer watermark.
	{
		const auto snapshot = *server.RoomSnapshot();
		room::Action chat;
		chat.kind = room::ActionKind::Chat;
		chat.roomEpoch = snapshot.roomEpoch;
		chat.revision = snapshot.revision;
		chat.actionId = 4;
		chat.text = "ACK watermark regression";
		protocol::RoomActionMessage message;
		message.action = chat;
		transport->Push(1, json(message));
		CHECK(server.Step() == 0);
		commitCandidate(true, "room_result", 1);
	}
	terminalAction(1, 3);
	// The exact duplicate still crosses the committed reply boundary, but the
	// already acknowledged receipt did not mutate the room and must not enqueue
	// another snapshot/projection burst for every member.
	commitCandidate(true, "room_result", 1, -1, 0, 1);
	// Even an action ID equal to the generic watermark cannot bypass the exact
	// terminal receipt checks.
	terminalAction(1, 4, interruptedGeneration + 100);
	commitCandidate(true, "room_result", 0, static_cast<int>(room::RejectReason::WrongGeneration), 0, 1);
	terminalAction(1, 4, interruptedGeneration, server.RoomSnapshot()->roomEpoch + 1);
	commitCandidate(true, "room_result", 0, static_cast<int>(room::RejectReason::StaleRoom), 0, 1);
	// Member two's ACK was allocated first but did not commit before a newer chat
	// action. The older exact ACK must still perform the receipt mutation.
	{
		const auto snapshot = *server.RoomSnapshot();
		room::Action chat;
		chat.kind = room::ActionKind::Chat;
		chat.roomEpoch = snapshot.roomEpoch;
		chat.revision = snapshot.revision;
		chat.actionId = 5;
		chat.text = "ACK committed after newer action";
		protocol::RoomActionMessage message;
		message.action = chat;
		transport->Push(2, json(message));
		CHECK(server.Step() == 0);
		commitCandidate(true, "room_result", 1);
	}
	terminalAction(2, 4);
	commitCandidate(true, "room_result", 1);

	// The next Ready pair receives a fresh generation after the committed
	// cancellation. The two no-effect acknowledgments below also commit native
	// authority progress before game_connect/game_start are released.
	action(1, room::ActionKind::Ready, 5);
	commitCandidate(true, "room_result");
	action(2, room::ActionKind::Ready, 6);
	commitCandidate(true, "game_prepare");
	auto generation = server.RoomSnapshot()->tables[0].matchGeneration;
	CHECK(generation > interruptedGeneration);
	auto acknowledge = [&](session::Connection connection, const char* type, std::uint64_t matchGeneration = 0) {
		transport->Push(connection, json{{"type", type}, {"generation", matchGeneration ? matchGeneration : generation}});
		CHECK(server.Step() == 0);
	};
	acknowledge(1, "game_prepared");
	commitCandidate(false);
	acknowledge(2, "game_prepared");
	commitCandidate(true, "game_connect");
	acknowledge(1, "game_ready");
	commitCandidate(false);
	acknowledge(2, "game_ready");
	commitCandidate(true, "game_start");
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CheckIdleRecoveryWork(server,"playing");
	// Gameplay diagnostics used to be journaled as committed effects, so every
	// 30 frames of a fight built and proposed a full room checkpoint that
	// fenced the room until each peer imported it. They carry no room
	// mutation: forward them directly, with no candidate and no commit token.
	{
		const auto hashBuilds = server.RecoveryCheckpointBuilds();
		for (int frame = 30; frame <= 240; frame += 30) {
			protocol::BattleHashV2 hash;
			hash.frameIdx = frame; hash.fromPlayer = true;
			transport->Push(1, json(hash));
		}
		CHECK(server.Step() == 0);
		CHECK(transport->incoming.empty());
		CHECK(!server.HasRecoveryCandidate());
		CHECK(server.PendingProposal() == nullptr);
		CHECK(server.RecoveryCheckpointBuilds() == hashBuilds);
		CHECK(transport->outgoing.size() == 8);
		CHECK(std::all_of(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& message) {
			return message.first == 2 && message.second.value("type", std::string()) == "battle_hash" &&
				!message.second.contains("_commit");
		}));
		transport->outgoing.clear();
	}
	// Keep another table in a healthy Started generation while table zero enters
	// result reconciliation. A pending or disputed result on one table must not
	// manufacture room-wide recovery work on every game-thread tick.
	admissionHello(3, "Gate-three");
	admissionHello(4, "Gate-four");
	action(3, room::ActionKind::Queue, 1, 1);
	commitCandidate(true, "room_result");
	action(4, room::ActionKind::Queue, 1, 1);
	commitCandidate(true, "room_result");
	action(3, room::ActionKind::Ready, 2, 1);
	commitCandidate(true, "room_result");
	action(4, room::ActionKind::Ready, 2, 1);
	commitCandidate(true, "game_prepare");
	const auto otherGeneration = server.RoomSnapshot()->tables[1].matchGeneration;
	acknowledge(3, "game_prepared", otherGeneration);
	commitCandidate(false);
	acknowledge(4, "game_prepared", otherGeneration);
	commitCandidate(true, "game_connect");
	acknowledge(3, "game_ready", otherGeneration);
	commitCandidate(false);
	acknowledge(4, "game_ready", otherGeneration);
	commitCandidate(true, "game_start");
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing &&
		server.RoomSnapshot()->tables[1].phase == room::TablePhase::Playing);
	// A same-term follower transition must freeze the relative result age even
	// when no new term is elected. The writable tick later rebases the clock
	// and preserves the five-second age rather than disputing after 40 seconds
	// of passive wall time.
	// The preceding term-loss recovery leaves the imported authority paused.
	// Rebase it before creating the result so the first five-second tick is a
	// real timer candidate rather than the initial writable resume.
	server.AdvanceCustomRoom(0);
	transport->outgoing.clear();
	const auto playing = *server.RoomSnapshot();
	room::Action finished;
	finished.kind = room::ActionKind::MatchFinished;
	finished.roomEpoch = playing.roomEpoch;
	finished.revision = playing.revision;
	finished.tableRevision = playing.tables[0].revision;
	finished.actionId = 100;
	finished.table = 0;
	finished.matchGeneration = playing.tables[0].matchGeneration;
	protocol::RoomActionMessage finishedMessage; finishedMessage.action = finished;
	transport->Push(1, json(finishedMessage));
	CHECK(server.Step() == 0);
	commitCandidate(true, "room_result");
	const auto pendingBuilds = server.RecoveryCheckpointBuilds();
	server.AdvanceCustomRoom(5000);
	CHECK(!server.HasRecoveryCandidate());
	CHECK(server.RecoveryCheckpointBuilds() == pendingBuilds);
	// Advancing the owner clock before the deadline is local bookkeeping. The
	// five-second age must still survive a follower interval without requiring
	// a private candidate or quorum proposal.
	auto timed = server.RecoveryCheckpoint();
	CHECK(timed.at("room").at("snapshot").at("tables").at(0).at("result_pending") == true);
	CHECK(timed.at("room").at("result_age").at(0) == 5000);
	server.SetAuthority(term, revision, false);
	server.AdvanceCustomRoom(45000);
	timed = server.RecoveryCheckpoint();
	CHECK(timed.at("room").at("result_age").at(0) == 5000);
	server.SetAuthority(term, revision, true);
	server.AdvanceCustomRoom(45000);
	timed = server.RecoveryCheckpoint();
	CHECK(timed.at("room").at("result_age").at(0) == 5000);
	// A Started generation survives another term transition and is not treated
	// as an interrupted preparation.
	term = 4;
	server.SetAuthority(term, revision, false);
	server.AdvanceCustomRoom(90000);
	timed = server.RecoveryCheckpoint();
	CHECK(timed.at("room").at("result_age").at(0) == 5000);
	server.SetAuthority(term, revision, true);
	CHECK(server.CancelInterruptedPreparations());
	CHECK(!server.HasRecoveryCandidate());
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(server.RoomSnapshot()->tables[1].phase == room::TablePhase::Playing);
	// Resume the five-second age at the new owner's clock, then cross the exact
	// thirty-second deadline once. Only that transition should create a
	// candidate, and the unrelated table must remain in its healthy fight.
	const auto resumedBuilds = server.RecoveryCheckpointBuilds();
	server.AdvanceCustomRoom(90000);
	CHECK(!server.HasRecoveryCandidate() && server.RecoveryCheckpointBuilds() == resumedBuilds);
	server.AdvanceCustomRoom(115000);
	CHECK(server.HasRecoveryCandidate());
	commitCandidate(true, "room_snapshot");
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Paused);
	CHECK(server.RoomSnapshot()->tables[0].resultPending);
	CHECK(server.RoomSnapshot()->tables[1].phase == room::TablePhase::Playing);
	CheckIdleRecoveryWork(server, "paused result beside playing table");
}

static void TestCustomRoomDepartures() {
	auto* transport = new MockTransport();
	SessionServer server("custom-room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> room = {};
	room[0] = 1;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Departure test", 16, 9);
	CHECK(server.Listen(0) == 0);
	auto step = [&]() { CHECK(server.Step() == 0); };
	auto hello = [&](session::Connection connection) {
		transport->Push(connection, json(protocol::SessionHelloMsg()));
		step();
		CHECK(server.cidMap.count(connection) == 1);
	};
	auto join = [&](session::Connection connection, const char* name) {
		protocol::SessionJoinRequest request;
		request.username = name;
		request.sidecarHash = "build";
		request.port = 30000;
		request.customRooms = true;
		request.roomProtocol = room::ProtocolVersion;
		transport->outgoing.clear();
		json requestJson=request;requestJson["mainFighter"]=static_cast<int>(connection)+8;
		transport->Push(connection, requestJson);
		step();
		CHECK(server.roomMembers.count(connection) == 1);
		for(const auto& member:server.RoomSnapshot()->members)if(member.id==server.roomMembers.at(connection))
			CHECK(json(member).value("main_fighter",-1)==static_cast<int>(connection)+8);
	};
	for (session::Connection connection = 1; connection <= 6; ++connection) {
		hello(connection);
		join(connection, (std::string("Member") + std::to_string(connection)).c_str());
	}
    hello(7);
    protocol::SessionJoinRequest profile;profile.username="Profile validation";profile.sidecarHash="build";
    profile.port=30000;profile.customRooms=true;profile.roomProtocol=room::ProtocolVersion;
    for(const json invalid:{json(-2),json(44),json(1.5),json("Ryu"),json(UINT64_MAX)}){
        json bad=profile;bad["mainFighter"]=invalid;
        transport->Push(7,bad);step();CHECK(server.roomMembers.count(7)==0);
    }
    json older=profile;older.erase("mainFighter");transport->Push(7,older);step();
    CHECK(server.roomMembers.count(7)==1&&server.RoomSnapshot()->members.back().mainFighter==-1);
    transport->disconnected.push_back(7);step();CHECK(server.roomMembers.count(7)==0);
	std::map<session::Connection, std::uint64_t> actionIds;
	auto action = [&](session::Connection connection, room::ActionKind kind, std::uint8_t table, room::MemberId target = 0) {
		room::Action value;
		const auto* snapshot = server.RoomSnapshot();
		value.kind = kind;
		value.roomEpoch = snapshot->roomEpoch;
		value.revision = snapshot->revision;
		value.table = table;
		value.tableRevision = snapshot->tables[table].revision;
		value.actionId = ++actionIds[connection];
		value.target = target;
		protocol::RoomActionMessage message;
		message.action = value;
		transport->outgoing.clear();
		transport->Push(connection, json(message));
		step();
	};
	auto hasMessage = [&](const char* type, session::Connection connection = 0) {
		return std::any_of(transport->outgoing.begin(), transport->outgoing.end(), [&](const auto& sent) {
			return sent.second.value("type", std::string()) == type && (!connection || sent.first == connection);
		});
	};

	// Start table 0 with a live spectator, then remove only that spectator.
	action(2, room::ActionKind::Queue, 0);
	action(3, room::ActionKind::Queue, 0);
	action(4, room::ActionKind::Watch, 0);
    // Existing pre-battle exchange publishes only validated fighter portraits.
    protocol::PreBattleSetChara portrait;portrait.chara=protocol::MatchData{}.chara[0];portrait.chara.charaID=2;
    transport->Push(2,json(portrait));step();
    const auto fighterOf=[&](session::Connection connection){
        for(const auto& m:server.RoomSnapshot()->members)if(m.id==server.roomMembers.at(connection))return m.fighter;
        return -2;
    };
    CHECK(fighterOf(2)==2&&fighterOf(3)==-1);
    CHECK(hasMessage("room_snapshot",6));
    CHECK(server.RoomSnapshot()->members[1].mainFighter==10);
    const auto shared=json(*server.RoomSnapshot()).get<room::Snapshot>();
    CHECK(shared.members[1].mainFighter==10&&shared.members[1].fighter==2);
    portrait.chara.charaID=44;transport->Push(2,json(portrait));step();CHECK(fighterOf(2)==2);
    portrait.chara.charaID=4;transport->Push(4,json(portrait));step();CHECK(fighterOf(4)==-1);
	action(2, room::ActionKind::Ready, 0);
	action(3, room::ActionKind::Ready, 0);
	const auto generation0 = server.RoomSnapshot()->tables[0].matchGeneration;
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(hasMessage("game_prepare", 2) && hasMessage("game_prepare", 3) && hasMessage("game_prepare", 4));
	auto acknowledge = [&](session::Connection connection, const char* type, std::uint64_t generation, json extra = json::object()) {
		transport->outgoing.clear();
		extra["type"] = type; extra["generation"] = generation;
		transport->Push(connection, extra);
		step();
	};
	// P1 (connection 2) accepts the optional-spectator start and reports the
	// spectator's link up, so the Started authority carries a start set that
	// the portable restore below must convert.
	acknowledge(2, "game_prepared", generation0, {{"spectators_optional", true}});
	for (session::Connection connection = 3; connection <= 4; ++connection) acknowledge(connection, "game_prepared", generation0);
	acknowledge(2, "game_ready", generation0, {{"slots", json::array({2})}});
	for (session::Connection connection = 3; connection <= 4; ++connection) acknowledge(connection, "game_ready", generation0);
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	action(4, room::ActionKind::Unwatch, 0);
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(hasMessage("game_peer_end", 2));
	CHECK(!hasMessage("game_end"));
	const auto spectatorMember = server.roomMembers.at(4);

	// A spectator removed after Started is absent from the public room roster,
	// but its frozen native slot must survive a portable restore and handle
	// permutation. The restored authority must remain Started so the two
	// fighters can continue without regenerating private capabilities.
	const auto portableCheckpoint = server.RecoveryCheckpoint();
	auto* portableTransport = new MockTransport();
	SessionServer portable("custom-room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(portableTransport));
	portable.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0'); identity[63] = "0123456789abcdef"[connection & 15]; return identity;
	});
	// Checkpoints no longer duplicate the proposal's pending effects, and one
	// written by an older owner that still carries them must restore.
	CHECK(!portableCheckpoint.contains("pending_effects"));
	auto legacyCheckpoint = portableCheckpoint;
	legacyCheckpoint["pending_effects"] = json::array();
	legacyCheckpoint["pending_effects_digest"] = std::string(64, '0');
	CHECK(portable.RestoreRecoveryCheckpoint(legacyCheckpoint));
	std::vector<SessionServer::StableRebind> restoredBindings;
	session::Connection reboundHandle = 200;
	session::Connection spectatorHandle = 0;
	for (const auto& row : portableCheckpoint.at("members")) {
		if (row.value("frozen", false)) continue;
		const auto data = row.at("data").get<protocol::MemberData>();
		const auto member = row.at("member").get<room::MemberId>();
		if (member == spectatorMember) spectatorHandle = reboundHandle;
		restoredBindings.emplace_back(member, reboundHandle++,
			data.connId, row.at("incarnation").get<std::uint64_t>());
	}
	CHECK(spectatorHandle && portable.RebindMembers(restoredBindings));
	const auto continued = portable.RecoveryCheckpoint();
	CHECK(continued.at("match_authorities").at(0).at("phase") == static_cast<int>(session::MatchAuthority::Phase::Started));
	CHECK(continued.at("match_authorities").at(0).at("spectators_optional") == true);
	CHECK(continued.at("match_authorities").at(0).at("start_spectators").size() == 1);
	CHECK(continued.at("match_authorities").at(0).at("participants").size() == 3);
	CHECK(portable.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(portableTransport->outgoing.empty());
	portable.SetAuthority(1, 0, true);
	room::Action leave;
	leave.kind = room::ActionKind::Leave;
	leave.roomEpoch = portable.RoomSnapshot()->roomEpoch;
	leave.revision = portable.RoomSnapshot()->revision;
	leave.actionId = 3;
	protocol::RoomActionMessage leaveMessage;
	leaveMessage.action = leave;
	portableTransport->Push(spectatorHandle, json(leaveMessage));
	CHECK(portable.Step() == 0 && portable.HasRecoveryCandidate());
	CHECK(portable.ProposeCheckpoint(1, 1, 0, portable.RecoveryCheckpoint()));
	const auto leaveProposal = portable.PendingProposal();
	CHECK(leaveProposal != nullptr);
	CHECK(portable.ApplyCommit(1, 1, 1, leaveProposal->checkpoint, leaveProposal->effectsDigest));
	CHECK(portable.roomFrozenMembers.size() == 1);
	const auto retained = portable.RecoveryCheckpoint();
	CHECK(std::count_if(retained.at("members").begin(), retained.at("members").end(),
		[](const json& member) { return member.value("frozen", false); }) == 1);
	CheckIdleRecoveryWork(portable, "retained started spectator");
	const auto stillRetained = portable.RecoveryCheckpoint();
	CHECK(std::count_if(stillRetained.at("members").begin(), stillRetained.at("members").end(),
		[](const json& member) { return member.value("frozen", false); }) == 1);

	// A recovery checkpoint includes native setup and match coordination, not
	// only the UI roster. Restoring it must not send grants or touch a socket.
	const auto checkpoint = json::parse(server.Checkpoint().dump());
	auto* recoveryTransport = new MockTransport();
	SessionServer recovery("custom-room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(recoveryTransport));
	recovery.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0'); identity[63] = "0123456789abcdef"[connection & 15]; return identity;
	});
	CHECK(recovery.RestoreCheckpoint(checkpoint));
	CHECK(recovery.Checkpoint() == checkpoint);
	CHECK(recoveryTransport->outgoing.empty() && !recoveryTransport->closed);
	CHECK(checkpoint.dump().find("capability") == std::string::npos);
	for (const char* key : {"room", "members", "cids", "room_members", "peer_identities", "match_authorities"}) {
		auto damaged = checkpoint; damaged[key] = nullptr;
		CHECK(!recovery.RestoreCheckpoint(damaged));
		CHECK(recovery.Checkpoint() == checkpoint);
	}
	// Duplicate native preparation acknowledgments after recovery cannot
	// restart a generation which has already reached Started.
	recoveryTransport->Push(2, json{{"type", "game_ready"}, {"generation", generation0}});
	CHECK(recovery.Step() == 0);
	CHECK(!recoveryTransport->Contains("game_start") && !recoveryTransport->Contains("game_prepare"));

	// A pre-start spectator disconnect ends only that table generation and
	// leaves the two fighters available for a fresh grant.
	action(5, room::ActionKind::Queue, 1);
	action(6, room::ActionKind::Queue, 1);
	action(4, room::ActionKind::Watch, 1);
	action(5, room::ActionKind::Ready, 1);
	action(6, room::ActionKind::Ready, 1);
	const auto generation1 = server.RoomSnapshot()->tables[1].matchGeneration;
	CHECK(server.RoomSnapshot()->tables[1].phase == room::TablePhase::Playing);
	CHECK(hasMessage("game_prepare", 4));
	transport->outgoing.clear();
	transport->disconnected.push_back(4);
	step();
	CHECK(hasMessage("game_end", 5) && hasMessage("game_end", 6));
	CHECK(server.RoomSnapshot()->tables[1].matchGeneration == generation1);
	CHECK(server.RoomSnapshot()->tables[1].phase == room::TablePhase::Waiting);
	CHECK(server.roomMembers.count(4) == 0 && server.ConnectedClientCount() == 5);

	// Kicking sends an observable rejection to the target, removes all of its
	// authenticated state, and blocks the same endpoint from rejoining.
	hello(7);
	join(7, "Kicked");
	const auto kickedMember = server.roomMembers.at(7);
	action(1, room::ActionKind::Kick, 0, kickedMember);
	CHECK(hasMessage("room_result", 7));
	const auto kickedResult = std::find_if(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& sent) {
		return sent.first == 7 && sent.second.value("type", std::string()) == "room_result";
	});
	CHECK(kickedResult != transport->outgoing.end());
	CHECK(!kickedResult->second.at("result").at("accepted").get<bool>());
	CHECK(kickedResult->second.at("result").at("reason").get<int>() == static_cast<int>(room::RejectReason::MemberKicked));
	CHECK(server.roomMembers.count(7) == 0 && server.cidMap.count(7) == 0);
	hello(7);
	protocol::SessionJoinRequest banned;
	banned.username = "Rejoined"; banned.sidecarHash = "build"; banned.port = 30000;
	banned.customRooms = true; banned.roomProtocol = room::ProtocolVersion;
	transport->outgoing.clear(); transport->Push(7, json(banned)); step();
	CHECK(transport->Contains("join_rej"));

	// A fighter disconnect from a waiting, partially-ready table fills the
	// vacancy FIFO and clears readiness before the replacement can ready.
	hello(8); join(8, "FighterA");
	hello(9); join(9, "FighterB");
	hello(10); join(10, "Queued");
	action(8, room::ActionKind::Queue, 2);
	action(9, room::ActionKind::Queue, 2);
	action(10, room::ActionKind::Queue, 2);
	action(8, room::ActionKind::Ready, 2);
	CHECK(server.RoomSnapshot()->tables[2].ready[0]);
	transport->outgoing.clear();
	transport->disconnected.push_back(8);
	step();
	const auto& recovered = server.RoomSnapshot()->tables[2];
	CHECK(recovered.p1 == server.roomMembers.at(10) && recovered.p2 == server.roomMembers.at(9));
	CHECK(!recovered.ready[0] && !recovered.ready[1] && recovered.phase == room::TablePhase::Waiting);
	server.Close();
}

int main() {
	auto* transport = new MockTransport();
	SessionServer server("room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	CHECK(server.Listen(0) == 0);
	auto step = [&]() { CHECK(server.Step() == 0); };
	auto hello = [&](session::Connection connection) {
		transport->Push(connection, json(protocol::SessionHelloMsg()));
		step();
		CHECK(server.cidMap.count(connection) == 1);
	};
	auto join = [&](session::Connection connection, const char* name, const char* hash = "build") {
		protocol::SessionJoinRequest request;
		request.username = name;
		request.sidecarHash = hash;
		request.port = 30000;
		transport->outgoing.clear();
		transport->Push(connection, json(request));
		step();
	};
	hello(10000000001ULL); // The core must not truncate to a GNS handle.
	hello(2);
	join(10000000001ULL, "Host");
	join(2, "Guest", "wrong-build");
	CHECK(server.ConnectedClientCount() == 1);
	CHECK(transport->Contains("join_rej"));
	join(2, "Host");
	CHECK(server.ConnectedClientCount() == 1);
	join(2, "Guest");
	CHECK(server.ConnectedClientCount() == 2);
	join(2, "Duplicate");
	CHECK(server.ConnectedClientCount() == 2);
	CHECK(transport->Contains("join_rej"));

	protocol::LobbySetSettings settings;
	settings.roundCount = 5;
	settings.trainingMode = true;
	transport->Push(2, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 3);
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 5 && server._lobbyData.trainingMode);
	protocol::PreBattleSetStage stage;
	for (const auto& option : selection::StageList()) {
		stage.stageID = option.id;
		transport->Push(10000000001ULL, json(stage)); step();
		CHECK(server._matchData.stageID == option.id);
	}
	stage.stageID = 8;
	transport->Push(2, json(stage)); step();
	CHECK(server._matchData.stageID == 29); // Only the authenticated host chooses.
	for (json value : {json(-1), json(22), json(23), json(30), json(4294967296ULL), json(UINT64_MAX),
		json(1.5), json(1.0), json(true), json("1"), json(nullptr), json::array()}) {
		json request = stage; request["stageID"] = value;
		transport->Push(10000000001ULL, request); step();
		CHECK(server._matchData.stageID == 29);
		CHECK(selection::PreferenceStage(request, "stageID", 0) == 0);
	}
	json missingStage = stage; missingStage.erase("stageID");
	transport->Push(10000000001ULL, missingStage); step();
	CHECK(server._matchData.stageID == 29);

	// A selection belongs to the authenticated player. Reject malformed bytes
	// before narrowing and reject unsupported combinations before storing them.
	protocol::PreBattleSetChara selection;
	selection.chara = server._matchData.chara[1];
	selection.chara.charaID = 1;
	selection.chara.costume = 1;
	selection.chara.color = 2;
	transport->Push(2, json(selection)); step();
	const json guestPick = server._matchData.chara[1];
	const json hostPick = server._matchData.chara[0];
	CHECK(guestPick == json(selection.chara) && hostPick != guestPick);
	const std::pair<const char*, int> invalidOptions[] = {
		{"charaID", 44}, {"costume", 7}, {"color", 22}, {"unc_edition", 0},
		{"ultraCombo", 3}, {"personalAction", 10}, {"winQuote", 11}, {"handicap", 5}
	};
	for (const auto& option : invalidOptions) {
		json request = selection; request["chara"][option.first] = option.second;
		transport->Push(2, request); step();
		CHECK(json(server._matchData.chara[1]) == guestPick);
	}
	for (const char* field : {"charaID", "costume", "color", "unc_edition", "ultraCombo",
		"personalAction", "winQuote", "handicap", "_unused"}) {
		for (json value : {json(-1), json(256), json(1.5), json(true), json("1"), json(nullptr)}) {
			json request = selection; request["chara"][field] = value;
			transport->Push(2, request); step();
			CHECK(json(server._matchData.chara[1]) == guestPick);
		}
	}
	json unsupported = selection;
	unsupported["chara"]["unc_edition"] = 13;
	unsupported["chara"]["ultraCombo"] = 1; // SFIV has one Ultra.
	transport->Push(2, unsupported); step();
	CHECK(json(server._matchData.chara[1]) == guestPick);
	unsupported["chara"]["charaID"] = 43; // Decapre has no SFIV edition.
	unsupported["chara"]["ultraCombo"] = 0;
	transport->Push(2, unsupported); step();
	CHECK(json(server._matchData.chara[1]) == guestPick);
	transport->Push(2, json(protocol::LobbyReady()), 90); step();
	// P2 may ready first; P1 still sends its stage immediately before Ready.
	transport->Push(10000000001ULL, json(stage)); step();
	CHECK(server._matchData.stageID == 8);
	selection.chara.color = 3;
	transport->Push(2, json(selection)); step();
	CHECK(json(server._matchData.chara[1]) == guestPick);
	CHECK(json(server._matchData.chara[0]) == hostPick);
	transport->Push(10000000001ULL, json(protocol::LobbyReset())); step();
	transport->Push(10000000001ULL, json(protocol::LobbyReady()), 91); step();
	const auto lockedStage = server._matchData.stageID;
	stage.stageID = 25;
	transport->Push(10000000001ULL, json(stage)); step();
	CHECK(server._matchData.stageID == lockedStage);
	transport->Push(10000000001ULL, json(protocol::LobbyReset())); step();

	for (std::int64_t generation = 1; generation <= 50; ++generation) {
		transport->outgoing.clear();
		transport->Push(10000000001ULL, json(protocol::LobbyReady()), generation * 100);
		transport->Push(2, json(protocol::LobbyReady()), generation * 100 + 1);
		step();
		CHECK(server._matchData.readyMessageNum[0] == generation * 100);
		CHECK(server._matchData.readyMessageNum[1] == generation * 100 + 1);
		CHECK(transport->Contains("lobby_allready"));
		transport->Push(10000000001ULL, json(protocol::LobbyReset())); step();
		CHECK(!server._matchData.IsAllReady());
	}

	hello(3); join(3, "Spectator");
	hello(4); join(4, "Spectator2");
	hello(5); join(5, "Overflow");
	CHECK(server.ConnectedClientCount() == 4);
	CHECK(transport->Contains("join_rej"));
	transport->outgoing.clear();
	transport->Push(3, json(protocol::LobbyReady()), 9000); step();
	CHECK(!server._matchData.IsAllReady());
	protocol::LobbyReportResults results;
	results.loserSide = -1;
	transport->Push(2, json(results)); step();
	results.loserSide = 0;
	transport->Push(3, json(results)); step();
	CHECK(server.clients[0].conn == 10000000001ULL);

	// A forwarding source must match the authenticated connection even when
	// it claims a different host. The old path logged some forgeries then sent.
	protocol::ForwardMessage forward;
	forward.src = {"other-room", "2"};
	forward.dest = server.cidMap.at(10000000001ULL);
	transport->outgoing.clear();
	transport->Push(2, json(forward)); step();
	CHECK(transport->outgoing.empty());
	forward.src = server.cidMap.at(2);
	transport->Push(2, json(forward)); step();
	CHECK(transport->outgoing.size() == 1);
	CHECK(transport->outgoing[0].first == 10000000001ULL);

	transport->disconnected.push_back(2); step();
	CHECK(server.ConnectedClientCount() == 3 && server.cidMap.count(2) == 0);
	hello(2); join(2, "Returned");
	CHECK(server.ConnectedClientCount() == 4);
	std::array<std::uint8_t, 16> room = {}; room[0] = 1;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		return std::string(64, connection == 10000000001ULL ? '1' : static_cast<char>('0' + connection));
	});
	settings.trainingMode = false;
	settings.editionSelect = true;
	settings.roundCount = 7;
	settings.roundTime = {0, 99};
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	settings.roundCount = 2; settings.editionSelect = false;
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	settings.roundCount = 5; settings.roundTime = {1, 99};
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	settings.roundTime = {0, 99};
	transport->Push(3, json(settings)); step(); // Current P2 cannot edit.
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	json narrowed = settings;
	narrowed["roundTime"]["fractional"] = 65536;
	transport->Push(10000000001ULL, narrowed); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	narrowed = settings; narrowed["roundCount"] = 5.75;
	transport->Push(10000000001ULL, narrowed); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	transport->Push(10000000001ULL, json(protocol::LobbyReady()), 10000); step();
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect); // One ready player freezes settings.
	transport->outgoing.clear();
	transport->Push(3, json(protocol::LobbyReady()), 10001); step();
	CHECK(transport->Contains("game_prepare") && !transport->Contains("lobby_allready"));
	std::map<session::Connection, json> grants;
	for (const auto& sent : transport->outgoing) if (sent.second["type"] == "game_prepare") grants.emplace(sent.first, sent.second);
	CHECK(grants.size() == 4 && grants[10000000001ULL]["links"].size() == 3);
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect); // Connecting is also immutable.
	CHECK(grants[3]["links"].size() == 1 && grants[4]["links"].size() == 1);
	CHECK(grants[10000000001ULL]["links"][0]["capability"] == grants[3]["links"][0]["capability"]);
	CHECK(grants[3]["links"][0]["capability"] != grants[4]["links"][0]["capability"]);
	auto ack = [&](session::Connection connection, const char* type, std::uint64_t generation) {
		transport->Push(connection, json{{"type", type}, {"generation", generation}}); step();
	};
	transport->outgoing.clear();
	ack(5, "game_prepared", 1); // A rejected/non-member connection has no vote.
	ack(10000000001ULL, "game_prepared", 0);
	ack(10000000001ULL, "game_prepared", 1);
	ack(10000000001ULL, "game_prepared", 1); // Duplicates cannot count twice.
	ack(3, "game_prepared", 1); ack(4, "game_prepared", 1);
	CHECK(!transport->Contains("game_connect"));
	ack(2, "game_prepared", 1); CHECK(transport->Contains("game_connect"));
	transport->outgoing.clear();
	ack(10000000001ULL, "game_ready", 1); ack(3, "game_ready", 1); ack(4, "game_ready", 1);
	CHECK(!transport->Contains("game_start"));
	ack(2, "game_ready", 0); CHECK(!transport->Contains("game_start"));
	ack(2, "game_ready", 1); CHECK(transport->Contains("game_start"));
	transport->outgoing.clear();
	json report = protocol::LobbyReportResults(); report["loserSide"] = 0; report["generation"] = std::uint64_t(1);
	transport->Push(4, report); step(); CHECK(!transport->Contains("game_end"));
	report["generation"] = std::uint64_t(0); transport->Push(10000000001ULL, report); step(); CHECK(!transport->Contains("game_end"));
	report["generation"] = std::uint64_t(1); transport->Push(10000000001ULL, report); step(); CHECK(transport->Contains("game_end"));
	CHECK(transport->outgoing.size() == 8);
	for (std::size_t i = 0; i < 4; ++i) CHECK(transport->outgoing[i].second["type"] == "data_update");
	for (std::size_t i = 4; i < 8; ++i) CHECK(transport->outgoing[i].second["type"] == "game_end");
	CHECK(server.clients[0].conn == 3 && server.clients[1].conn == 4);
	transport->outgoing.clear();
	transport->Push(3, json(protocol::LobbyReady()), 11000); transport->Push(4, json(protocol::LobbyReady()), 11001); step();
	CHECK(transport->Contains("game_prepare"));
	transport->outgoing.clear();
	ack(3, "game_prepared", 1); ack(4, "game_prepared", 1); ack(2, "game_prepared", 1); ack(10000000001ULL, "game_prepared", 1);
	CHECK(!transport->Contains("game_connect"));
	transport->disconnected.push_back(4); step(); CHECK(transport->Contains("game_end"));
	transport->writable = false;
	server.ResetLobbyForRematch();
	CHECK(server.Step() == -1); // Bounded transport failure reaches the owner.
	server.Close();
	CHECK(transport->closed && server.cidMap.empty() && server.clients.empty());
	TestAtomicMatchRebind();
	TestOptionalSpectatorBarrier();
	TestRoomChatDelta();
	TestSameTermProposalPause();
	TestMaximumRoomResultBurst();
	TestTerminalAcknowledgmentBatch();
	TestTerminalReceiptReplay();
	TestRetiringSpectatorProjection();
	TestCommittedSessionGate();
	TestCustomRoomDepartures();
	std::cout << "Session server mock transport tests passed\n";
}
