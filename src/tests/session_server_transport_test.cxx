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

#define CHECK(condition) do { if (!(condition)) { \
	std::cerr << "Check failed at " << __LINE__ << ": " #condition << '\n'; \
	std::exit(1); } } while (false)

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
	const auto action = [&](session::Connection connection, room::ActionKind kind, std::uint64_t actionId) {
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
	auto acknowledge = [&](session::Connection connection, const char* type) {
		transport->Push(connection, json{{"type", type}, {"generation", generation}});
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
	// Gameplay diagnostics used to consume one full quorum proposal each,
	// filling the router while the native fight kept producing new frames.
	// A diagnostic prefix now shares one bounded commit; the result boundary
	// following it still obeys ordinary authority validation.
	for (int frame = 30; frame <= 240; frame += 30) {
		protocol::BattleHashV2 hash;
		hash.frameIdx = frame; hash.fromPlayer = true;
		transport->Push(1, json(hash));
	}
	CHECK(server.Step() == 0);
	CHECK(transport->incoming.empty());
	commitCandidate(true, "battle_hash", -1, -1, -1, 8);
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
	server.AdvanceCustomRoom(5000);
	CHECK(server.HasRecoveryCandidate());
	// Commit the timer candidate before toggling authority. Otherwise the
	// private candidate short-circuits AdvanceCustomRoom and would make the
	// non-writable interval assertion vacuous.
	commitCandidate(false);
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
	auto acknowledge = [&](session::Connection connection, const char* type, std::uint64_t generation) {
		transport->outgoing.clear();
		transport->Push(connection, json{{"type", type}, {"generation", generation}});
		step();
	};
	for (session::Connection connection = 2; connection <= 4; ++connection) acknowledge(connection, "game_prepared", generation0);
	for (session::Connection connection = 2; connection <= 4; ++connection) acknowledge(connection, "game_ready", generation0);
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	action(4, room::ActionKind::Unwatch, 0);
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(hasMessage("game_peer_end", 2));
	CHECK(!hasMessage("game_end"));

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
	CHECK(portable.RestoreRecoveryCheckpoint(portableCheckpoint));
	std::vector<SessionServer::StableRebind> restoredBindings;
	session::Connection reboundHandle = 200;
	for (const auto& row : portableCheckpoint.at("members")) {
		if (row.value("frozen", false)) continue;
		const auto data = row.at("data").get<protocol::MemberData>();
		restoredBindings.emplace_back(row.at("member").get<room::MemberId>(), reboundHandle++,
			data.connId, row.at("incarnation").get<std::uint64_t>());
	}
	CHECK(portable.RebindMembers(restoredBindings));
	const auto continued = portable.RecoveryCheckpoint();
	CHECK(continued.at("match_authorities").at(0).at("phase") == static_cast<int>(session::MatchAuthority::Phase::Started));
	CHECK(continued.at("match_authorities").at(0).at("participants").size() == 3);
	CHECK(portable.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(portableTransport->outgoing.empty());

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
	TestSameTermProposalPause();
	TestMaximumRoomResultBurst();
	TestTerminalAcknowledgmentBatch();
	TestCommittedSessionGate();
	TestCustomRoomDepartures();
	std::cout << "Session server mock transport tests passed\n";
}
