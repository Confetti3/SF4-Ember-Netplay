// SessionServer: custom-room tables, per-table match authorities, projections and room broadcasts.

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

void SessionServer::EnableCustomRooms(const std::string& name, std::uint8_t capacity,
	std::uint64_t roomEpoch, room::Rules defaults) {
	if (_roomAuthority || !name.size()) return;
	_roomAuthority.reset(new room::RoomAuthority(name, capacity, roomEpoch, defaults));
	_roomMatchData.clear();
	for (auto& authority : _roomMatchAuthorities) authority.reset();
	if (_matchAuthorizationConfigured) {
		for (auto& authority : _roomMatchAuthorities) {
			authority.reset(new session::MatchAuthority(_matchAuthorizationRoom, _matchAuthorizationIdentity));
		}
	}
}

sf4e::session::MatchAuthority* SessionServer::RoomMatchAuthority(std::uint8_t table) {
	return table < room::TableCount ? _roomMatchAuthorities[table].get() : nullptr;
}

const sf4e::session::MatchAuthority* SessionServer::RoomMatchAuthority(std::uint8_t table) const {
	return table < room::TableCount ? _roomMatchAuthorities[table].get() : nullptr;
}

std::uint8_t SessionServer::RoomTableForGeneration(std::uint64_t generation) const {
	if (_roomAuthority) {
		for (std::uint8_t table = 0; table < room::TableCount; ++table) {
			if (_roomAuthority->SnapshotView().tables[table].matchGeneration == generation &&
				_roomAuthority->SnapshotView().tables[table].phase == room::TablePhase::Playing) return table;
		}
	}
	for (std::uint8_t table = 0; table < room::TableCount; ++table) {
		if (_roomMatchAuthorities[table] && _roomMatchAuthorities[table]->Generation() == generation &&
			_roomMatchAuthorities[table]->GetPhase() != session::MatchAuthority::Phase::Idle) return table;
	}
	return static_cast<std::uint8_t>(room::TableCount);
}

bool SessionServer::IsRoomTableParticipant(session::Connection connection, std::uint8_t tableId) const {
	if (!_roomAuthority || tableId >= room::TableCount) return false;
	const auto local = roomMembers.find(connection);
	if (local == roomMembers.end()) return false;
	const auto& table = _roomAuthority->SnapshotView().tables[tableId];
	return table.p1 == local->second || table.p2 == local->second ||
		std::find(table.spectators.begin(), table.spectators.end(), local->second) != table.spectators.end();
}

void SessionServer::SendRoomTable(std::uint8_t tableId, const json& message) {
	if (!_roomAuthority || tableId >= room::TableCount) return;
	const auto& table = _roomAuthority->SnapshotView().tables[tableId];
	std::set<session::Connection> destinations;
	const auto add = [&](room::MemberId id) {
		if (!id) return;
		for (const auto& member : roomMembers) {
			if (member.second == id) { destinations.insert(member.first); break; }
		}
	};
	add(table.p1); add(table.p2);
	for (const auto spectator : table.spectators) add(spectator);
	for (const auto connection : destinations) Respond(connection, message);
}

bool SessionServer::BeginAuthorizedTable(std::uint8_t tableId, std::uint64_t generation) {
	if (!_roomAuthority || !_matchAuthorizationConfigured || tableId >= room::TableCount) return false;
	auto* authority = RoomMatchAuthority(tableId);
	if (!authority) return false;
	const auto& snapshot = _roomAuthority->SnapshotView();
	const auto& table = snapshot.tables[tableId];
	if (table.phase != room::TablePhase::Playing || table.matchGeneration != generation ||
		table.p1 == 0 || table.p2 == 0) return false;
	const auto findMember = [&](room::MemberId id) -> const room::Member* {
		for (const auto& member : snapshot.members) if (member.id == id) return &member;
		return nullptr;
	};
	const auto connectionFor = [&](room::MemberId id) -> session::Connection {
		for (const auto& pair : roomMembers) if (pair.second == id) return pair.first;
		return 0;
	};
	std::vector<session::MatchAuthority::Participant> participants;
	std::set<room::MemberId> seen;
	const auto add = [&](room::MemberId id) {
		if (!id || !seen.insert(id).second) return false;
		const auto* member = findMember(id);
		if (!member) return false;
		const auto connection = connectionFor(id);
		const auto cid = cidMap.find(connection);
		if (!connection || cid == cidMap.end()) return false;
		participants.push_back({connection, cid->second});
		return true;
	};
	// The roster RoomAuthority froze for this generation: the fighters, then
	// every spectator not still retiring an earlier generation.
	const auto roster = _roomAuthority->LiveMatchRoster(tableId);
	if (roster.size() < 2) return false;
	for (const auto member : roster) if (!add(member)) return false;
	// Send the immutable native projection before any prepare grant. Clients
	// freeze their legacy game buffers when that grant arrives.
	for (const auto& participant : participants) ProjectRoomTable(participant.connection, tableId);
	return authority->BeginAtGeneration(participants, generation, MatchSender());
}

void SessionServer::AdvanceCustomRoom(std::uint64_t nowMs) {
	if (!_roomAuthority) return;
	if (_recovery.Enabled() && !_recovery.Writable()) {
		if (!_roomAuthority->RecoveryPaused()) _roomAuthority->PauseForRecovery();
		if (_coordinationHealthy) {
			if (_passiveTimerClock && nowMs >= _passiveTimerClock)
				_roomAuthority->AdvancePausedTimers(nowMs - _passiveTimerClock);
			_passiveTimerClock = nowMs;
		} else _passiveTimerClock = 0;
		return;
	}
	if (_recovery.Enabled() && _roomAuthority->RecoveryPaused()) {
		if (_coordinationHealthy && _passiveTimerClock && nowMs >= _passiveTimerClock)
			_roomAuthority->AdvancePausedTimers(nowMs - _passiveTimerClock);
		_roomAuthority->ResumeRecovery(nowMs);
	}
	_passiveTimerClock = 0;
	if (_recovery.Enabled() && (_recoveryCandidateReady || _recovery.PendingProposal())) return;
	// Before a result deadline, AdvanceTime can only advance the local clock.
	// Keep that clock current without serializing a full checkpoint merely to
	// rediscover the unchanged room. RoomAuthority owns the deadline rules so a
	// paused unresolved result does not masquerade as recurring timer work.
	if (_recovery.Enabled()) {
		if (!_roomAuthority->HasDueTimerTransition(nowMs)) {
			_roomAuthority->AdvanceTime(nowMs);
			return;
		}
	}
	if (_recovery.Enabled()) {
		BeginRecoveryCandidate();
		if (!_recoveryCandidateReady) return;
	}
	const auto events = _roomAuthority->AdvanceTime(nowMs);
	if (!events.empty()) BroadcastRoomState(events);
	// Timer-driven table transitions can end an authority without a control
	// action.  Drop frozen spectator records only after every authority has
	// released the native slot, so a rematch on another table remains able to
	// rebind the departed endpoint.
	PruneFrozenMembers();
	if (_recovery.Enabled() && _recoveryCandidateReady && events.empty() && _candidate.effects.empty()) {
		// AdvanceTime records a monotonic clock even when no deadline fired. It
		// is not a room mutation worth a quorum round; preserve relative result
		// ages, however, by retaining any candidate whose room state changed.
		try {
			const auto current = RecoveryCheckpoint();
			json beforeRoom = _recoveryBaseline.value("room", json::object());
			json currentRoom = current.value("room", json::object());
			beforeRoom["time"] = 0;
			currentRoom["time"] = 0;
			// The first owner tick rebases a paused checkpoint to its local
			// monotonic clock. That bookkeeping is not a room outcome; compare
			// the stable state and relative ages only.
			for (auto* roomState : {&beforeRoom, &currentRoom}) {
				(*roomState)["recovery_paused"] = false;
				(*roomState)["result_since"] = json::array();
				(*roomState)["chat_times"] = json::array();
			}
			if (beforeRoom == currentRoom) DropRecoveryCandidate();
		} catch (...) { /* retain the candidate and fail closed */ }
	}
	if (_recovery.Enabled()) FinishRecoveryCandidate();
}

void SessionServer::SendRoomProjection(session::Connection connection) {
	if (!_roomAuthority) return;
	const auto member = roomMembers.find(connection);
	if (member == roomMembers.end()) return;
	const auto selected = roomSelectedTables.find(connection);
	const std::uint8_t tableId = selected == roomSelectedTables.end() ? 0 : selected->second;
	ProjectRoomTable(connection, tableId);
}

void SessionServer::ProjectRoomTable(session::Connection connection, std::uint8_t tableId) {
	if (!_roomAuthority || tableId >= room::TableCount) return;
	const auto member = roomMembers.find(connection);
	if (member == roomMembers.end()) return;
	const auto& snapshot = _roomAuthority->SnapshotView();
	const auto& table = snapshot.tables[tableId];
	SessionProtocol::SessionDataUpdate update;
	update.matchGeneration = table.matchGeneration;
	update.lobbyData = _lobbyData;
	update.lobbyData.members.clear();
	update.lobbyData.editionSelect = table.rules.editionSelect;
	update.lobbyData.roundCount = table.rules.roundCount;
	update.lobbyData.roundTime.integral = table.rules.roundTime;
	update.lobbyData.roundTime.fractional = 0;
	update.lobbyData.trainingMode = false;
	const auto dataFor = [&](room::MemberId id) -> const room::Member* {
		for (const auto& item : snapshot.members) if (item.id == id) return &item;
		return nullptr;
	};
	const auto cidFor = [&](const room::Member& roomMember) -> SessionProtocol::ConnectionID {
		for (const auto& mapping : roomMembers) if (mapping.second == roomMember.id) {
			const auto cid = cidMap.find(mapping.first);
			if (cid != cidMap.end()) return cid->second;
		}
		return {roomMember.connection.host, roomMember.connection.user};
	};
	for (const auto id : {table.p1, table.p2}) {
		if (!id) {
			update.lobbyData.members.push_back(SessionProtocol::MemberData{});
			continue;
		}
		const auto* roomMember = dataFor(id);
		if (!roomMember) continue;
		SessionProtocol::MemberData data;
		data.connId = cidFor(*roomMember);
		data.name = roomMember->name;
		data.ip.clear(); data.port = 0; data.flags = 0;
		update.lobbyData.members.push_back(data);
	}
	// While a generation is live its native roster is the one BeginMatch froze
	// and the grant carries. table.spectators can also hold a member still
	// retiring the previous generation, which AcceptGrant would reject.
	const auto live = _roomAuthority->LiveMatchRoster(tableId);
	const std::vector<room::MemberId> spectators = live.empty()
		? table.spectators : std::vector<room::MemberId>(live.begin() + 2, live.end());
	for (const auto id : spectators) {
		const auto* roomMember = dataFor(id);
		if (!roomMember) continue;
		SessionProtocol::MemberData data;
		data.connId = cidFor(*roomMember);
		data.name = roomMember->name;
		data.ip.clear(); data.port = 0; data.flags = 0;
		update.lobbyData.members.push_back(data);
	}
	update.matchData = _roomMatchData[tableId];
	// The native protocol keeps one delay per seat, and each client applies
	// the entry for its own slot. Custom rooms fill both with the shared match
	// delay, so every client plays at it without a protocol change.
	update.matchData.inputDelay[0] = update.matchData.inputDelay[1] = room::MatchDelay(table);
	update.authorityTerm = _recovery.Authority().term;
	update.authorityRevision = _recovery.Authority().revision;
	Respond(connection, json(update));
}

std::uint8_t SessionServer::RoomTableFor(session::Connection connection) const {
	const auto selected = roomSelectedTables.find(connection);
	return selected == roomSelectedTables.end() || selected->second >= room::TableCount ? 0 : selected->second;
}

int SessionServer::RoomSideFor(session::Connection connection, std::uint8_t tableId) const {
	if (!_roomAuthority || tableId >= room::TableCount) return -1;
	const auto roomMember = roomMembers.find(connection);
	if (roomMember == roomMembers.end()) return -1;
	const auto& table = _roomAuthority->SnapshotView().tables[tableId];
	return table.p1 == roomMember->second ? 0 : table.p2 == roomMember->second ? 1 : -1;
}

SessionServer::ChatVersion SessionServer::CurrentChatVersion() const {
	ChatVersion version;
	if (!_roomAuthority) return version;
	const auto& chat = _roomAuthority->SnapshotView().chat;
	version.count = chat.size();
	version.last = chat.empty() ? 0 : chat.back().sequence;
	return version;
}

void SessionServer::EnableChatDelta(session::Connection connection, bool enabled) {
	for (auto& client : clients) if (client.conn == connection) { client.chatDelta = enabled; client.chatSent = false; }
}

void SessionServer::BroadcastRoomState(const std::vector<room::Event>& events) {
	if (!_roomAuthority) return;
	diag::ScopedTimer timer(diag::OP_ROOM_BROADCAST);
	const auto chat = CurrentChatVersion();
	for (auto& client : clients) {
		const auto member = roomMembers.find(client.conn);
		if (member == roomMembers.end()) continue;
		SessionProtocol::RoomSnapshotMessage snapshot;
		snapshot.snapshot = _roomAuthority->SnapshotFor(member->second);
		// Room chat is most of a snapshot and rarely changes. A client that
		// accepts it gets the chat only when its committed copy is stale.
		snapshot.chatUnchanged = client.chatDelta && client.chatSent && client.chatVersion == chat;
		if (snapshot.chatUnchanged) snapshot.snapshot.chat.clear();
		else if (client.chatDelta) {
			// Counts as held once it commits; until then a later snapshot in the
			// same candidate, which supersedes this one, carries the chat too.
			if (_recovery.Enabled() && !_recoveryFlushing) _candidate.chatSent[client.conn] = chat;
			else { client.chatSent = true; client.chatVersion = chat; }
		}
		Respond(client.conn, json(snapshot));
		for (const auto& event : events) {
			// The snapshot already carries roster/chat changes. Wire events are
			// reserved for lifecycle notifications, and table notifications are
			// scoped to the member's selected table to keep 16-member bursts
			// within the helper queue bound.
			if (event.kind == room::Event::Kind::SnapshotChanged ||
				event.kind == room::Event::Kind::ChatMessage ||
				event.kind == room::Event::Kind::MemberRemoved) continue;
			if (event.kind == room::Event::Kind::MatchEnded && event.terminalReplay) {
				const auto recipients = _roomAuthority->TerminalMembers(event.table, event.matchGeneration);
				if (std::find(recipients.begin(), recipients.end(), member->second) == recipients.end()) continue;
			}
			if (!(event.kind == room::Event::Kind::MatchEnded && event.terminalReplay) &&
				event.kind != room::Event::Kind::RoomClosed && RoomTableFor(client.conn) != event.table) continue;
			SessionProtocol::RoomEventMessage eventMessage;
			eventMessage.event = event;
			Respond(client.conn, json(eventMessage));
		}
		SendRoomProjection(client.conn);
	}
}
