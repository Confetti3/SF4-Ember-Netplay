#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <exception>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "sf4e__SessionProtocol.hxx"
#include "sf4e__SessionServer.hxx"
#include "RoomMessageQueue.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
namespace session = sf4e::session;
namespace room = sf4e::room;
namespace diag = sf4e::diag;
using Dimps::Math::FixedPoint;
using sf4e::SessionServer;

namespace {
bool IsRecoveryBatchableMessage(const session::Message& message) {
	// Diagnostic prefixes have no room mutation. Commit a bounded ordered
	// batch rather than one quorum round-trip per verification frame.
	if (session::IsRoomVerificationMessage(message)) return true;
	try {
		const auto payload = json::parse(message.payload);
		SessionProtocol::MessageType type;
		payload.at("type").get_to(type);
		if (type != SessionProtocol::MT_ROOM_ACTION) return false;
		SessionProtocol::RoomActionMessage action;
		payload.get_to(action);
		return action.action.protocolVersion == room::ProtocolVersion &&
			(action.action.kind == room::ActionKind::RecordResult ||
				action.action.kind == room::ActionKind::AcknowledgeTerminal);
	} catch (const std::exception&) {
		return false;
	}
}
}


const int sf4e::SESSION_SERVER_MAX_MESSAGES_PER_POLL = 200;

SessionServer::SessionServer(std::string identity, std::string sidecarHash, bool editionSelect, int roundCount, FixedPoint roundTime,
	std::unique_ptr<session::ServerTransport> transport) :
	_identity(identity),
	_sidecarHash(sidecarHash),
	_transport(std::move(transport)),
	_dataDirty(false),
	_lobbyData(SessionProtocol::LobbyData::NULL_LOBBY)
{
	_lobbyData.id = { _identity, "1" };
	_lobbyData.editionSelect = editionSelect;
	_lobbyData.roundCount = roundCount;
	_lobbyData.roundTime = roundTime;
	clients.reserve(MAX_SF4E_PROTOCOL_USERS + 1);
	_transportFailed = !_transport;
}

SessionServer::~SessionServer()
{
	Close();
}

void SessionServer::CaptureFrozenMember(room::MemberId member, const room::Snapshot& prior) {
	if (!member) return;
	const auto priorMember = std::find_if(prior.members.begin(), prior.members.end(),
		[&](const room::Member& value) { return value.id == member; });
	if (priorMember == prior.members.end()) return;
	bool frozenSlot = false;
	const SessionProtocol::ConnectionID endpoint{priorMember->connection.host, priorMember->connection.user};
	for (std::uint8_t table = 0; table < room::TableCount; ++table) {
		const auto* authority = _roomMatchAuthorities[table].get();
		if (authority && authority->HasStartedSpectator(endpoint)) {
			frozenSlot = true;
			break;
		}
	}
	if (!frozenSlot) return;
	FrozenMember frozen;
	frozen.endpoint = priorMember->connection;
	frozen.data.connId = {frozen.endpoint.host, frozen.endpoint.user};
	frozen.data.name = priorMember->name;
	frozen.data.roomMember = member;
	frozen.data.authenticatedEndpoint = roomPeerIdentities.count(member) ? roomPeerIdentities.at(member) : frozen.endpoint.user;
	frozen.incarnation = roomIncarnations.count(member) ? roomIncarnations.at(member) : 1;
	frozen.data.incarnation = frozen.incarnation;
	for (const auto& client : clients) {
		const auto mapping = roomMembers.find(client.conn);
		if (mapping != roomMembers.end() && mapping->second == member) {
			frozen.data.ip = client.data.ip;
			frozen.data.port = client.data.port;
			frozen.data.flags = client.data.flags;
			break;
		}
	}
	roomFrozenMembers[member] = std::move(frozen);
}

void SessionServer::PruneFrozenMembers() {
	for (auto frozen = roomFrozenMembers.begin(); frozen != roomFrozenMembers.end();) {
		const SessionProtocol::ConnectionID endpoint{frozen->second.endpoint.host, frozen->second.endpoint.user};
		bool referenced = false;
		for (const auto& authority : _roomMatchAuthorities)
			if (authority && authority->HasStartedSpectator(endpoint)) { referenced = true; break; }
		if (!referenced && _matchAuthority && _matchAuthority->HasStartedSpectator(endpoint)) referenced = true;
		if (referenced) ++frozen;
		else frozen = roomFrozenMembers.erase(frozen);
	}
}

int SessionServer::Listen(uint16_t port) {
	return _transport && _transport->Listen(port) ? 0 : -1;
}

bool SessionServer::IsStaleMatchAck(const session::Message& message) const {
	if (message.payload.find("\"game_") == std::string::npos) return false;
	try {
		const auto payload = json::parse(message.payload);
		const auto type = payload.value("type", std::string());
		if (type != "game_prepared" && type != "game_ready") return false;
		const auto* authority = _roomAuthority
			? RoomMatchAuthority(RoomTableForGeneration(payload.value("generation", std::uint64_t(0))))
			: _matchAuthority.get();
		return !authority || !authority->Expects(message.connection, payload);
	} catch (const std::exception&) { return false; }
}

int SessionServer::Step()
{
	if (_recovery.Enabled()) {
		if (!_recovery.Writable()) return 0;
		// A timer tick may already have opened a private candidate.  Continue
		// polling and append later commands to that same candidate while it has
		// not been proposed; once the helper has a pending proposal the candidate
		// is immutable until the quorum result arrives.
		if (_recovery.PendingProposal()) return 0;
	}
	std::vector<session::Message> messages;
	std::vector<session::Connection> closed;
	const bool polled = !_transportFailed && _transport && (_recovery.Enabled()
		? _transport->PollRecoveryPrefix(messages, closed, room::MaximumMembers,
			[](const session::Message& message) { return IsRecoveryBatchableMessage(message); })
		: _transport->Poll(messages, closed, static_cast<std::size_t>(SESSION_SERVER_MAX_MESSAGES_PER_POLL)));
	if (!polled) {
		return -1;
	}
	// Verification (battle_hash / battle_snapshot) never mutates room state.
	// Forward it to the other table participants straight away, before any
	// recovery candidate exists: journaling it used to turn every 30 frames of
	// a fight into a full checkpoint proposal that fenced the whole room.
	// Delivery is best effort; a peer whose control socket is gone simply
	// misses a frame, as it did when journaling skipped it.
	for (auto it = messages.begin(); it != messages.end();) {
		if (!session::IsRoomVerificationMessage(*it)) { ++it; continue; }
		const auto conn = it->connection;
		const auto table = _roomAuthority ? RoomTableFor(conn) : static_cast<std::uint8_t>(room::TableCount);
		const bool live = cidMap.find(conn) != cidMap.end() && (!_roomAuthority || (RoomMatchAuthority(table) &&
			RoomMatchAuthority(table)->GetPhase() != session::MatchAuthority::Phase::Idle && IsRoomTableParticipant(conn, table)));
		if (live && _transport)
			for (const auto& client : clients)
				if (client.conn != conn && (!_roomAuthority || IsRoomTableParticipant(client.conn, table)))
					_transport->Send(client.conn, it->payload);
		it = messages.erase(it);
	}
	// A match acknowledgement its authority no longer expects changes nothing.
	// With optional spectators, a spectator's game_prepared/game_ready routinely
	// arrives after the fighters have moved on; opening a recovery candidate for
	// each would build a full checkpoint per spectator for no mutation.
	if (_recovery.Enabled())
		messages.erase(std::remove_if(messages.begin(), messages.end(),
			[&](const session::Message& message) { return IsStaleMatchAck(message); }), messages.end());
	if (_recovery.Enabled()) {
		// Polling only transfers the bounded inbox; it does not mutate native
		// session state. Capture the baseline before processing actual work,
		// including deferred output. Frozen members are retained data, not pending
		// cleanup: every command which can release their native authority reaches
		// the PruneFrozenMembers boundary below. An empty inbox must not manufacture
		// and compare two full recovery checkpoints.
		if (!_recoveryCandidateReady && messages.empty() && closed.empty() &&
			!_dataDirty && _afterDataMessages.empty()) return 0;
		BeginRecoveryCandidate();
		if (!_recoveryCandidateReady) return -1;
	}
	for (auto connection : closed) {
		_departingConnections.insert(connection);
		cidMap.erase(connection);
		if (_roomAuthority) {
			const auto roomMember = roomMembers.find(connection);
			const auto priorSnapshot = _roomAuthority->SnapshotCopy();
			std::vector<room::Event> departureEvents;
			for (std::uint8_t table = 0; table < room::TableCount; ++table) {
				auto* authority = RoomMatchAuthority(table);
				if (authority) {
					if (!authority->MemberDeparted(connection, MatchSender())) _transportFailed = true;
				}
				// MatchAuthority tears down the native generation first. End the
				// corresponding room generation before Leave clears the seat, so a
				// fighter disconnect cannot strand the table in Paused.
				if (roomMember != roomMembers.end() && priorSnapshot.host != roomMember->second) {
					const auto& oldTable = priorSnapshot.tables[table];
					const bool fighter = oldTable.p1 == roomMember->second || oldTable.p2 == roomMember->second;
					const bool matchEnded = authority == nullptr || authority->GetPhase() == session::MatchAuthority::Phase::Idle;
					if (matchEnded && (fighter || oldTable.spectators.end() != std::find(oldTable.spectators.begin(), oldTable.spectators.end(), roomMember->second)) &&
						(oldTable.phase == room::TablePhase::Playing || oldTable.phase == room::TablePhase::Paused ||
						oldTable.phase == room::TablePhase::Ready)) {
						auto ended = _roomAuthority->EndMatch(table, oldTable.matchGeneration, room::MatchResult::Abort);
						if (ended.accepted) departureEvents.insert(departureEvents.end(), ended.events.begin(), ended.events.end());
					}
				}
			}
			if (roomMember != roomMembers.end()) {
				CaptureFrozenMember(roomMember->second, priorSnapshot);
				auto leave = _roomAuthority->Leave(roomMember->second);
				roomPeerIdentities.erase(roomMember->second);
				roomMembers.erase(roomMember);
				roomSelectedTables.erase(connection);
				if (leave.accepted) {
					departureEvents.insert(departureEvents.end(), leave.events.begin(), leave.events.end());
					BroadcastRoomState(departureEvents);
				}
			}
		}
		for (auto member = clients.begin(); member != clients.end(); ++member) {
			if (member->conn == connection) {
				clients.erase(member);
				if (_matchAuthority) _matchAuthority->MemberDeparted(MatchSender());
				_matchData.ClearReady(); ResetBattleSync(); _dataDirty = true;
				_punchReady[0] = _punchReady[1] = false;
				break;
			}
		}
		for (auto& loaded : _roomBattleLoaded) loaded.erase(connection);
		for (auto& punch : _roomPunchReady) punch.erase(connection);
		_departingConnections.erase(connection);
	}
	bool bSendLobbyAllReady = false;
	bool bSendBattleSynced = false;
	std::vector<room::Event> deferredRoomEvents;
	for (const auto& incoming : messages) {
		const auto conn = incoming.connection;
		json msg;
		try {
			msg = json::parse(incoming.payload);
		}
		catch (const json::exception&) {
			spdlog::info("Server: got a non-json message");
			continue;
		}

		SessionProtocol::MessageType type;
		try {
			msg.at("type").get_to(type);
		}
		catch (const json::exception&) {
			spdlog::info("Server: got a message without a type, or a type that was not a string");
			continue;
		}

		if (cidMap.find(conn) == cidMap.end()) {
			if (type == SessionProtocol::MT_SESSION_HELLO) {
				HandleSessionHello(conn, msg, incoming);
			}
			else if (type == SessionProtocol::MT_ROOM_ACTION) {
				// A retried Leave that arrives after the first one removed its sender.
				spdlog::info("Server: ignoring a room action from a connection that already left");
			}
			else {
				spdlog::warn("Server: got unrecognized message type: {}", (int)type);
			}
		}
		else {
			SessionProtocol::ConnectionID cid = cidMap[conn];
			const auto name = msg.value("type", std::string());
			if (type == SessionProtocol::MT_ROOM_ACTION && _roomAuthority) {
				HandleRoomAction(conn, msg, deferredRoomEvents);
				continue;
			}
			if (name == "game_prepared" || name == "game_ready") {
				HandleMatchAcknowledgement(conn, msg);
				continue;
			}
			if (type == SessionProtocol::MT_FORWARD) {
				HandleForward(conn, msg, cid);
			}
			else if (type == SessionProtocol::MT_SESSION_JOINREQ) {
				HandleJoinRequest(conn, msg, incoming, cid);
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETCHARA) {
				HandleSetChara(conn, msg);
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETENV) {
				HandleSetEnv(conn, msg);
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETSTAGE) {
				HandleSetStage(conn, msg);
			}
			else if (type == SessionProtocol::MT_LOBBY_SETSETTINGS) {
				HandleLobbySetSettings(conn, msg);
			}
			else if (type == SessionProtocol::MT_BATTLE_LOADED) {
				HandleBattleLoaded(conn, bSendBattleSynced);
			}
			else if (type == SessionProtocol::MT_LOBBY_READY) {
				HandleLobbyReady(conn, msg, incoming, bSendLobbyAllReady);
			}
			else if (type == SessionProtocol::MT_LOBBY_REPORTRESULTS) {
				HandleLobbyReportResults(conn, msg);
			}
			else if (type == SessionProtocol::MT_LOBBY_RESET) {
				HandleLobbyReset(conn, msg);
			}
			else if (type == SessionProtocol::MT_PUNCH_READY) {
				HandlePunchReady(conn);
			}
			else if (type == SessionProtocol::MT_BATTLE_GGPO_FRAME) {
				HandleGgpoFrame(conn, msg, cid);
			}
			else {
				spdlog::warn("Server: got unrecognized message type: {}", (int)type);
			}
		}
	}

	bool roomStateBroadcast = false;
	if (_roomAuthority && !deferredRoomEvents.empty()) {
		BroadcastRoomState(deferredRoomEvents);
		roomStateBroadcast = true;
	}
	if (_dataDirty) {
		if (_roomAuthority) {
			if (!roomStateBroadcast) BroadcastRoomState();
			_dataDirty = false;
			// Custom snapshots carry the complete room roster; the legacy
			// update below would incorrectly expose all tables as one match.
		} else {
		SessionProtocol::SessionDataUpdate updateMsg;
		updateMsg.lobbyData = _lobbyData;
		updateMsg.matchData = _matchData;
		updateMsg.lobbyData.members.clear();
		for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
			updateMsg.lobbyData.members.push_back(clientIter->data);
		}
		BroadcastMessage(json(updateMsg));
		_dataDirty = false;
		}
	}

	// Roster/settings updates precede match-end acknowledgments. A client may
	// enable Ready only after it knows which role it owns in the next match.
	for (const auto& pending : _afterDataMessages) {
		if (std::none_of(clients.begin(), clients.end(), [&](const SessionMember& member) { return member.conn == pending.first; })) continue;
		Respond(pending.first, pending.second);
	}
	_afterDataMessages.clear();
	if (bSendLobbyAllReady) {
		if (_matchAuthority) {
			std::vector<session::MatchAuthority::Participant> participants;
			for (const auto& member : clients) participants.push_back({member.conn, member.data.connId});
			if (!_matchAuthority->Begin(participants, MatchSender())) _transportFailed = true;
		} else BroadcastMessage(json(SessionProtocol::LobbyAllReady()));
	}

	if (bSendBattleSynced) {
		BroadcastMessage(json(SessionProtocol::BattleSynced()));
	}

	// Room actions and connection closes can end the last authority reference
	// during this poll.  Keep frozen spectators while any Started authority
	// still owns their native slot, then prune the derived cache at the command
	// boundary.
	PruneFrozenMembers();
	if (_recovery.Enabled()) FinishRecoveryCandidate();
	return _transportFailed ? -1 : 0;
}

void SessionServer::Respond(session::Connection client, const nlohmann::json& msg, bool retainPublicReplay) {
	if (_recovery.Enabled() && !_recoveryFlushing) { JournalEffect(client, msg, retainPublicReplay); return; }
	if (!_transport || !_transport->Send(client, msg.dump())) _transportFailed = true;
}

void SessionServer::EnableMatchAuthorization(std::array<std::uint8_t, 16> room, session::MatchAuthority::Identity identity,
	std::function<std::uint64_t(session::Connection)> incarnation) {
	_matchAuthorizationRoom = room;
	_matchAuthorizationIdentity = std::move(identity);
	_memberIncarnation = std::move(incarnation);
	_matchAuthorizationConfigured = true;
	_matchAuthority.reset(new session::MatchAuthority(_matchAuthorizationRoom, _matchAuthorizationIdentity));
	if (_roomAuthority) {
		for (auto& authority : _roomMatchAuthorities) {
			authority.reset(new session::MatchAuthority(_matchAuthorizationRoom, _matchAuthorizationIdentity));
		}
	}
}

sf4e::session::MatchAuthority::Send SessionServer::MatchSender() {
	return [this](session::Connection connection, const json& message) {
		// A departed control peer may retain healthy gameplay, but is no longer
		// a destination for room coordination.
		if (_departingConnections.count(connection)) return true;
		if (std::none_of(clients.begin(), clients.end(), [&](const SessionMember& member) { return member.conn == connection; })) return true;
		if (message.at("type") == "game_end") {
			if (_recovery.Enabled() && !_recoveryFlushing) { JournalEffect(connection, message); return true; }
			const auto queueLimit = _roomAuthority ? room::MaxMembers * room::TableCount : MAX_SF4E_PROTOCOL_USERS;
			if (_afterDataMessages.size() >= queueLimit) { _transportFailed = true; return false; }
			_afterDataMessages.emplace_back(connection, message); return true;
		}
		if (_recovery.Enabled() && !_recoveryFlushing) { JournalEffect(connection, message); return true; }
		const bool sent = _transport && _transport->Send(connection, message.dump());
		if (!sent) _transportFailed = true;
		return sent;
	};
}

void SessionServer::BroadcastMessage(const nlohmann::json& msg) {
	for (const auto& client : clients) {
		Respond(client.conn, msg);
	}
}

int SessionServer::Close()
{
	if (_transport) _transport->Close();
	clients.clear();
	cidMap.clear();
	roomMembers.clear();
	roomSelectedTables.clear();
	roomPeerIdentities.clear();
	roomFrozenMembers.clear();
	roomBannedIdentities.clear();
	_departingConnections.clear();
	return 0;
}

void SessionServer::PrepareForCallbacks()
{
	// Legacy adapters own callback routing; the lobby has no global instance.
}

void SessionServer::ResetBattleSync()
{
	for (int i = 0; i < clients.size(); i++) {
		clients.at(i).data.flags &= (~SessionProtocol::MF_BATTLE_LOADED);
	}
}

void SessionServer::ResetLobbyForRematch()
{
	// Custom rooms are driven by committed RoomAction records. The legacy
	// callback must never mutate a passive/custom authority outside Step().
	if (_roomAuthority) return;
	if (_matchAuthority && !_matchAuthority->End(MatchSender())) _transportFailed = true;
	_matchData.ClearReady();
	ResetBattleSync();
	_punchReady[0] = false;
	_punchReady[1] = false;
	_dataDirty = true;
}

SessionProtocol::JoinResult SessionServer::RegisterToWait(
	const session::Connection& conn,
	const uint16_t& port,
	const std::string& sidecarHash,
	const std::string& name,
	const std::string& peerAddr,
	SessionProtocol::ConnectionID& cid,
	int mainFighter
) {
	if (sidecarHash != _sidecarHash) {
		return SessionProtocol::JR_HASH_INVALID;
	}
	for (const auto& iter : clients) {
		if (iter.conn == conn) {
			if (_memberIncarnation && _memberIncarnation(conn) != iter.data.incarnation)
				return SessionProtocol::JR_REQUEST_INVALID;
			// A recovery bootstrap hello is followed by the legacy join_req from
			// old clients. Treat the exact same authenticated profile as an
			// idempotent projection request rather than creating a second member.
			if (iter.data.name == name && iter.data.port == port && iter.data.ip == peerAddr) return SessionProtocol::JOIN_OK;
			return SessionProtocol::JR_REQUEST_INVALID;
		}
		if (iter.data.name == name) return SessionProtocol::JR_NAME_TAKEN;
	}
	if (_roomAuthority) {
		const bool isHost = conn == 1;
		std::string peerIdentity = cid.user;
		if (_matchAuthorizationConfigured && _matchAuthorizationIdentity) {
			const auto stable = _matchAuthorizationIdentity(conn);
			if (!stable.empty()) peerIdentity = stable;
		}
		if (roomBannedIdentities.count(peerIdentity)) return SessionProtocol::JR_REQUEST_INVALID;
		const auto incarnation = _memberIncarnation ? _memberIncarnation(conn) : _incarnation;
		if (!incarnation) return SessionProtocol::JR_REQUEST_INVALID;
		// Member.connection remains the protocol CID. The stable peer identity
		// above is only an admission/kick ban key.
		const room::ConnectionRef connectionRef{cid.host, cid.user};
		auto joined = _roomAuthority->Join(name, connectionRef, isHost, mainFighter);
		if (!joined.accepted) {
			switch (joined.reason) {
			case room::RejectReason::RoomFull: return SessionProtocol::JR_LOBBY_FULL;
			case room::RejectReason::NameTaken: return SessionProtocol::JR_NAME_TAKEN;
			case room::RejectReason::AdmissionLocked: return SessionProtocol::JR_LOBBY_FULL;
			default: return SessionProtocol::JR_REQUEST_INVALID;
			}
		}
		room::MemberId memberId = 0;
		for (const auto& item : joined.snapshot.members) if (item.connection == connectionRef) { memberId = item.id; break; }
		if (!memberId) return SessionProtocol::JR_REQUEST_INVALID;
		roomMembers[conn] = memberId;
		roomPeerIdentities[memberId] = std::move(peerIdentity);
		roomIncarnations[memberId] = incarnation;
		_roomAuthority->SetMemberIncarnation(memberId, incarnation);
		roomSelectedTables[conn] = 0;
	}

	if (clients.size() >= MAX_SF4E_PROTOCOL_USERS) {
		if (!_roomAuthority) return SessionProtocol::JR_LOBBY_FULL;
	}

	SessionMember newMember{ {cid, name, peerAddr, port}, conn };
	if (_roomAuthority) {
		const auto id = roomMembers.at(conn);
		newMember.data.roomMember = id;
		newMember.data.authenticatedEndpoint = roomPeerIdentities.at(id);
		newMember.data.incarnation = roomIncarnations.at(id);
	}
	clients.push_back(std::move(newMember));
	return SessionProtocol::JOIN_OK;
}

void SessionServer::HandleResults(int loserIndex) {
	if (_matchAuthority && !_matchAuthority->End(MatchSender())) _transportFailed = true;
	auto loser = clients.begin() + loserIndex;
	clients.push_back(*loser);
	clients.erase(loser);
	_matchData.Clear();
}
