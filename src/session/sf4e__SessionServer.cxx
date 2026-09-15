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

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
namespace session = sf4e::session;
namespace room = sf4e::room;
using Dimps::Math::FixedPoint;
using sf4e::SessionServer;

namespace {
constexpr std::size_t kRecoveryProposalBytes = 1024 * 1024;

session::Connection SyntheticConnection(room::MemberId member) {
	return (std::numeric_limits<session::Connection>::max)() - member;
}

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

json SessionServer::Checkpoint() const {
	if (!_roomAuthority || !_departingConnections.empty() || !_afterDataMessages.empty())
		throw std::logic_error("room checkpoint requires a completed command boundary");
	json members = json::array(), authorities = json::array();
	for (const auto& member : clients)
		members.push_back({{"connection", member.conn}, {"data", member.data}, {"flags", member.data.flags}});
	for (const auto& authority : _roomMatchAuthorities)
		authorities.push_back(authority ? authority->Checkpoint() : json(nullptr));
	json lobby = _lobbyData;
	for (std::size_t i = 0; i < _lobbyData.members.size(); ++i)
		lobby["members"][i]["flags"] = _lobbyData.members[i].flags;
	json value = {{"version", 1}, {"identity", _identity}, {"build", _sidecarHash},
		{"room", _roomAuthority->Checkpoint()}, {"members", members}, {"cids", cidMap},
		{"room_members", roomMembers}, {"selected_tables", roomSelectedTables},
		{"peer_identities", roomPeerIdentities}, {"bans", roomBannedIdentities},
		{"lobby", lobby}, {"match", _matchData}, {"table_matches", _roomMatchData},
		{"battle_loaded", _roomBattleLoaded}, {"punch_ready", _roomPunchReady},
		{"legacy_punch_ready", _punchReady}, {"dirty", _dataDirty},
		{"authorized", _matchAuthorizationConfigured}, {"authorization_room", _matchAuthorizationRoom},
		{"match_authorities", authorities},
		{"legacy_authority", _matchAuthority ? _matchAuthority->Checkpoint() : json(nullptr)}};
	if (value.dump().size() > room::RoomAuthority::MaximumCheckpointBytes)
		throw std::length_error("room checkpoint exceeds replication limit");
	return value;
}

bool SessionServer::RestoreCheckpoint(const json& value) {
	try {
		if (value.dump().size() > room::RoomAuthority::MaximumCheckpointBytes || value.at("version") != 1 ||
			value.at("identity") != _identity || value.at("build") != _sidecarHash ||
			value.at("authorized") != _matchAuthorizationConfigured ||
			value.at("authorization_room").get<decltype(_matchAuthorizationRoom)>() != _matchAuthorizationRoom)
			return false;
		// The temporary owns no transport. Rejected imports cannot partially
		// replace live state, disconnect peers, or publish native match effects.
		SessionServer candidate(_identity, _sidecarHash, true, 3, {0, 99}, nullptr);
		candidate._roomAuthority.reset(new room::RoomAuthority("Recovering room"));
		if (!candidate._roomAuthority->RestoreCheckpoint(value.at("room"))) return false;
		const auto& snapshot = candidate._roomAuthority->SnapshotView();
		const auto readMap = [&](const char* key, auto& destination, std::size_t maximum) {
			const auto& rows = value.at(key);
			if (!rows.is_array() || rows.size() > maximum) return false;
			for (const auto& row : rows) {
				if (!row.is_array() || row.size() != 2 || !row[0].is_number_unsigned()) return false;
				using Map = typename std::decay<decltype(destination)>::type;
				const auto keyValue = row[0].get<typename Map::key_type>();
				if (static_cast<std::uint64_t>(keyValue) != row[0].get<std::uint64_t>() ||
					!destination.emplace(keyValue, row[1].get<typename Map::mapped_type>()).second) return false;
			}
			return true;
		};
		if (!readMap("cids", candidate.cidMap, room::MaxMembers * 2) ||
			!readMap("room_members", candidate.roomMembers, room::MaxMembers) ||
			!readMap("selected_tables", candidate.roomSelectedTables, room::MaxMembers) ||
			!readMap("peer_identities", candidate.roomPeerIdentities, room::MaxMembers) ||
			!readMap("table_matches", candidate._roomMatchData, room::TableCount)) return false;
		std::set<std::pair<std::string, std::string>> cids;
		for (const auto& mapping : candidate.cidMap) {
			const auto& cid = mapping.second;
			if (!mapping.first || cid.host != _identity || cid.user.empty() || cid.user.size() > 256 ||
				!cids.emplace(cid.host, cid.user).second) return false;
		}
		std::set<room::MemberId> memberIds;
		for (const auto& mapping : candidate.roomMembers) {
			const auto member = std::find_if(snapshot.members.begin(), snapshot.members.end(),
				[&](const room::Member& m) { return m.id == mapping.second; });
			const auto cid = candidate.cidMap.find(mapping.first);
			if (member == snapshot.members.end() || cid == candidate.cidMap.end() ||
				member->connection.host != cid->second.host || member->connection.user != cid->second.user ||
				!memberIds.insert(mapping.second).second) return false;
		}
		if (memberIds.size() != snapshot.members.size()) return false;
		for (const auto& mapping : candidate.roomSelectedTables)
			if (!candidate.roomMembers.count(mapping.first) || mapping.second >= room::TableCount) return false;
		for (const auto& mapping : candidate._roomMatchData)
			if (mapping.first >= room::TableCount) return false;
		std::set<std::string> identities;
		for (const auto& mapping : candidate.roomPeerIdentities) {
			if (!memberIds.count(mapping.first) || mapping.second.empty() || mapping.second.size() > 256 ||
				!identities.insert(mapping.second).second) return false;
		}
		if (candidate.roomPeerIdentities.size() != memberIds.size()) return false;
		const auto& bans = value.at("bans");
		if (!bans.is_array()) return false;
		for (const auto& entry : bans) {
			const auto identity = entry.get<std::string>();
			if (identity.empty() || identity.size() > 256 || identities.count(identity) ||
				!candidate.roomBannedIdentities.insert(identity).second) return false;
		}
		const auto& members = value.at("members");
		if (!members.is_array() || members.size() != memberIds.size()) return false;
		std::set<session::Connection> connections;
		for (const auto& row : members) {
			if (!row.at("connection").is_number_unsigned() || !row.at("flags").is_number_unsigned()) return false;
			SessionMember member{};
			member.conn = row.at("connection").get<session::Connection>();
			member.data = row.at("data").get<SessionProtocol::MemberData>();
			member.data.flags = row.at("flags").get<std::uint64_t>();
			if (!candidate.roomMembers.count(member.conn) || !connections.insert(member.conn).second ||
				!(candidate.cidMap.at(member.conn) == member.data.connId) ||
				member.data.name.size() > 256 || member.data.ip.size() > 256 ||
				(member.data.flags & ~std::uint64_t(SessionProtocol::MF_BATTLE_LOADED))) return false;
			candidate.clients.push_back(std::move(member));
		}
		const auto readSets = [&](const char* key, auto& destination) {
			const auto& rows = value.at(key);
			if (!rows.is_array() || rows.size() != room::TableCount) return false;
			for (std::size_t i = 0; i < room::TableCount; ++i) {
				if (!rows[i].is_array() || rows[i].size() > room::MaxMembers) return false;
				for (const auto& entry : rows[i]) {
					if (!entry.is_number_unsigned()) return false;
					const auto connection = entry.get<session::Connection>();
					if (!connections.count(connection) || !destination[i].insert(connection).second) return false;
				}
			}
			return true;
		};
		if (!readSets("battle_loaded", candidate._roomBattleLoaded) ||
			!readSets("punch_ready", candidate._roomPunchReady)) return false;
		candidate._lobbyData = value.at("lobby").get<SessionProtocol::LobbyData>();
		if (candidate._lobbyData.id.host != _identity || candidate._lobbyData.members.size() > room::MaxMembers) return false;
		for (std::size_t i = 0; i < candidate._lobbyData.members.size(); ++i)
			candidate._lobbyData.members[i].flags = value.at("lobby").at("members")[i].at("flags").get<std::uint64_t>();
		candidate._matchData = value.at("match").get<SessionProtocol::MatchData>();
		const auto punch = value.at("legacy_punch_ready").get<std::array<bool, 2>>();
		candidate._dataDirty = value.at("dirty").get<bool>();
		const auto& authorities = value.at("match_authorities");
		if (!authorities.is_array() || authorities.size() != room::TableCount) return false;
		const auto restoreAuthority = [&](const json& data, std::unique_ptr<session::MatchAuthority>& authority) {
			if (data.is_null()) return !_matchAuthorizationConfigured;
			if (!_matchAuthorizationConfigured) return false;
			authority.reset(new session::MatchAuthority(_matchAuthorizationRoom, _matchAuthorizationIdentity));
			return authority->RestoreCheckpoint(data);
		};
		if (!restoreAuthority(value.at("legacy_authority"), candidate._matchAuthority)) return false;
		for (std::size_t i = 0; i < room::TableCount; ++i)
			if (!restoreAuthority(authorities[i], candidate._roomMatchAuthorities[i])) return false;
		_roomAuthority = std::move(candidate._roomAuthority);
		clients = std::move(candidate.clients); cidMap = std::move(candidate.cidMap);
		roomMembers = std::move(candidate.roomMembers); roomSelectedTables = std::move(candidate.roomSelectedTables);
		roomPeerIdentities = std::move(candidate.roomPeerIdentities); roomBannedIdentities = std::move(candidate.roomBannedIdentities);
		roomFrozenMembers.clear();
		_lobbyData = std::move(candidate._lobbyData); _matchData = candidate._matchData;
		_roomMatchData = std::move(candidate._roomMatchData);
		_roomBattleLoaded = std::move(candidate._roomBattleLoaded); _roomPunchReady = std::move(candidate._roomPunchReady);
		_matchAuthority = std::move(candidate._matchAuthority); _roomMatchAuthorities = std::move(candidate._roomMatchAuthorities);
		_punchReady[0] = punch[0]; _punchReady[1] = punch[1]; _dataDirty = candidate._dataDirty;
		_departingConnections.clear(); _afterDataMessages.clear();
		return true;
	} catch (const std::exception&) { return false; }
}

json SessionServer::RecoveryCheckpoint() const {
	++_recoveryCheckpointBuilds;
	if (!_roomAuthority || !_departingConnections.empty() || !_afterDataMessages.empty())
		throw std::logic_error("recovery checkpoint requires a completed command boundary");
	json value = Checkpoint();
	// The portable candidate carries relative timer ages. Pause a copy so
	// constructing a proposal cannot mutate the live owner before quorum.
	room::RoomAuthority pausedRoom = *_roomAuthority;
	pausedRoom.PauseForRecovery();
	value["room"] = pausedRoom.Checkpoint();
	value["schema"] = "session-recovery-v2";
	value["incarnation"] = _incarnation;
	json stableMembers = json::array();
	const auto findClient = [&](room::MemberId id) -> const SessionMember* {
		for (const auto& row : clients) {
			auto mapping = roomMembers.find(row.conn);
			if (mapping != roomMembers.end() && mapping->second == id) return &row;
		}
		return nullptr;
	};
	for (const auto& member : _roomAuthority->SnapshotView().members) {
		SessionProtocol::MemberData data;
		data.connId = {member.connection.host, member.connection.user};
		data.name = member.name;
		data.roomMember = member.id;
		data.authenticatedEndpoint = roomPeerIdentities.count(member.id) ? roomPeerIdentities.at(member.id) : member.connection.user;
		data.incarnation = roomIncarnations.count(member.id) ? roomIncarnations.at(member.id) : 1;
		const auto* client = findClient(member.id);
		if (client) {
			data.ip = client->data.ip; data.port = client->data.port; data.flags = client->data.flags;
		}
		stableMembers.push_back({{"member", member.id}, {"endpoint", member.connection}, {"data", data}, {"incarnation", data.incarnation}});
	}
	for (const auto& frozen : roomFrozenMembers) {
		if (std::find_if(_roomAuthority->SnapshotView().members.begin(), _roomAuthority->SnapshotView().members.end(),
			[&](const room::Member& value) { return value.id == frozen.first; }) != _roomAuthority->SnapshotView().members.end()) continue;
		stableMembers.push_back({{"member", frozen.first}, {"endpoint", frozen.second.endpoint},
			{"data", frozen.second.data}, {"incarnation", frozen.second.incarnation}, {"frozen", true}});
	}
	value["members"] = std::move(stableMembers);
	value.erase("cids"); value.erase("room_members"); value.erase("selected_tables");
	value.erase("battle_loaded"); value.erase("punch_ready");
	json selected = json::array();
	for (const auto& row : roomSelectedTables) {
		auto member = roomMembers.find(row.first);
		if (member != roomMembers.end()) selected.push_back({{"member", member->second}, {"table", row.second}});
	}
	value["selected_members"] = std::move(selected);
	const auto encodeConnections = [&](const auto& sets) {
		json result = json::array();
		for (const auto& set : sets) {
			json members = json::array();
			for (const auto connection : set) {
				auto member = roomMembers.find(connection);
				if (member != roomMembers.end()) members.push_back(member->second);
			}
			result.push_back(std::move(members));
		}
		return result;
	};
	value["battle_loaded_members"] = encodeConnections(_roomBattleLoaded);
	value["punch_ready_members"] = encodeConnections(_roomPunchReady);
	json portableAuthorities = json::array();
	for (const auto& authority : _roomMatchAuthorities)
		portableAuthorities.push_back(authority ? authority->PortableCheckpoint() : json(nullptr));
	value["match_authorities"] = std::move(portableAuthorities);
	value["legacy_authority"] = _matchAuthority ? _matchAuthority->PortableCheckpoint() : json(nullptr);
	for (auto& row : value["lobby"]["members"]) {
		const auto endpoint = row.value("connId", SessionProtocol::ConnectionID{});
		for (const auto& member : _roomAuthority->SnapshotView().members) {
			if (member.connection.host == endpoint.host && member.connection.user == endpoint.user) {
				row["roomMember"] = member.id;
				row["authenticatedEndpoint"] = roomPeerIdentities.count(member.id) ? roomPeerIdentities.at(member.id) : member.connection.user;
				row["incarnation"] = roomIncarnations.count(member.id) ? roomIncarnations.at(member.id) : 1;
				break;
			}
		}
	}
	value["authority"] = {{"term", _recovery.Authority().term}, {"revision", _recovery.Authority().revision}, {"writable", _recovery.Authority().writable}};
	value["effect_journal"] = _committedEffectHistory;
	value["pending_effects"] = _recoveryEffects;
	value["pending_effects_digest"] = session::EffectsDigest(_recoveryEffects);
	return value;
}

bool SessionServer::RestoreRecoveryCheckpoint(const json& value) {
	try {
		if (value.value("schema", std::string()) != "session-recovery-v2" || !value.at("room").is_object() ||
			!value.at("members").is_array() || value.at("members").size() > room::MaximumMembers + room::TableCount * room::MaxMatchParticipants) return false;
		const auto incomingHistory = value.value("effect_journal", std::vector<session::EffectEnvelope>{});
		if (incomingHistory.size() > session::MaxEffectJournalEntries || json(incomingHistory).dump().size() > session::MaxEffectJournalBytes) return false;
		for (const auto& effect : incomingHistory)
			if (effect.term == 0 || effect.recipient == 0 || !session::recovery_detail::IsSha256(effect.payloadDigest)) return false;
		json legacy = value;
		legacy.erase("schema"); legacy.erase("authority"); legacy.erase("effect_journal"); legacy.erase("incarnation");
		json legacyMembers = json::array(), cids = json::array(), roomMemberRows = json::array(), selected = json::array();
		std::map<room::MemberId, session::Connection> synthetic;
		std::map<room::MemberId, SessionProtocol::ConnectionID> endpoints;
		std::array<std::set<room::MemberId>, room::TableCount> pendingBattleLoaded{};
		std::array<std::set<room::MemberId>, room::TableCount> pendingPunchReady{};
		for (const auto& row : value.at("members")) {
			const auto member = row.at("member").get<room::MemberId>();
			const auto endpoint = row.at("endpoint").get<room::ConnectionRef>();
			if (!member || !synthetic.emplace(member, SyntheticConnection(member)).second) return false;
			SessionProtocol::MemberData data = row.at("data").get<SessionProtocol::MemberData>();
			data.connId = {endpoint.host, endpoint.user}; data.roomMember = member;
			data.authenticatedEndpoint = row.value("authenticated_endpoint", data.authenticatedEndpoint);
			data.incarnation = row.value("incarnation", data.incarnation ? data.incarnation : 1ULL);
			if (!row.value("frozen", false)) {
				legacyMembers.push_back({{"connection", synthetic.at(member)}, {"data", data}, {"flags", data.flags}});
				cids.push_back({synthetic.at(member), data.connId});
				roomMemberRows.push_back({synthetic.at(member), member});
			}
			endpoints.emplace(member, data.connId);
		}
		legacy["members"] = std::move(legacyMembers); legacy["cids"] = std::move(cids); legacy["room_members"] = std::move(roomMemberRows);
		for (const auto& row : value.value("selected_members", json::array())) {
			const auto member = row.at("member").get<room::MemberId>();
			if (!synthetic.count(member) || row.at("table").get<std::uint64_t>() >= room::TableCount) return false;
			selected.push_back({synthetic.at(member), row.at("table")});
		}
		legacy["selected_tables"] = std::move(selected);
		const auto readPendingSets = [&](const char* key, auto& target) {
			const auto rows = value.value(key, json::array());
			if (!rows.is_array() || rows.size() != room::TableCount) return false;
			for (std::size_t i = 0; i < room::TableCount; ++i) {
				if (!rows[i].is_array()) return false;
				for (const auto& id : rows[i]) {
					const auto member = id.get<room::MemberId>();
					if (!synthetic.count(member) || !target[i].insert(member).second) return false;
				}
			}
			return true;
		};
		if (!readPendingSets("battle_loaded_members", pendingBattleLoaded) || !readPendingSets("punch_ready_members", pendingPunchReady)) return false;
		const auto decodeSets = [&](const char* key, const char* output) {
			json result = json::array();
			for (const auto& members : value.value(key, json::array())) {
				json row = json::array();
				if (!members.is_array()) return json();
				for (const auto memberValue : members) {
					const auto member = memberValue.get<room::MemberId>();
					if (!synthetic.count(member)) return json(); row.push_back(synthetic.at(member));
				}
				result.push_back(std::move(row));
			}
			legacy[output] = std::move(result); return legacy[output];
		};
		if (decodeSets("battle_loaded_members", "battle_loaded").is_null() || decodeSets("punch_ready_members", "punch_ready").is_null()) return false;
		const auto convertAuthority = [&](const json& portable) {
			if (portable.is_null()) return json(nullptr);
			json authority = portable;
			json rows = json::array();
			for (const auto& row : portable.at("participants")) {
				const auto endpoint = row.at("endpoint").get<SessionProtocol::ConnectionID>();
				auto iter = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& p) { return p.second == endpoint; });
				if (iter == endpoints.end()) return json();
				rows.push_back({{"connection", synthetic.at(iter->first)}, {"member", endpoint}});
			}
			authority["participants"] = std::move(rows);
			for (const char* key : {"acknowledgments", "departed"}) {
				json mapped = json::array();
				for (const auto& endpointValue : portable.at(key)) {
					const auto endpoint = endpointValue.get<SessionProtocol::ConnectionID>();
					auto iter = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& p) { return p.second == endpoint; });
					if (iter == endpoints.end()) return json(); mapped.push_back(synthetic.at(iter->first));
				}
				authority[key] = std::move(mapped);
			}
			return authority;
		};
		json authorities = json::array();
		for (const auto& authority : value.at("match_authorities")) { auto converted = convertAuthority(authority); if (converted.is_discarded()) return false; authorities.push_back(std::move(converted)); }
		legacy["match_authorities"] = std::move(authorities);
		legacy["legacy_authority"] = convertAuthority(value.value("legacy_authority", json(nullptr)));
		if (legacy["legacy_authority"].is_discarded()) return false;
		const auto previousAuthorityTerm = _recovery.Authority().term;
		if (!RestoreCheckpoint(legacy)) return false;
		if (_roomAuthority && !_roomAuthority->RecoveryPaused()) _roomAuthority->PauseForRecovery();
		_committedEffectHistory = incomingHistory;
		_incarnation = value.value("incarnation", 1ULL);
		const auto authority = value.value("authority", json::object());
		if (!authority.empty()) {
			const auto importedTerm = authority.value("term", 0ULL);
			_recovery.SetAuthority(importedTerm, authority.value("revision", 0ULL), authority.value("writable", false));
			if (importedTerm && importedTerm != previousAuthorityTerm) _preparationCancellationRequested = true;
		}
		roomIncarnations.clear();
		roomFrozenMembers.clear();
		for (const auto& row : value.at("members")) {
			const auto member = row.at("member").get<room::MemberId>();
			const auto incarnation = row.value("incarnation", 1ULL);
			roomIncarnations[member] = incarnation;
			if (row.value("frozen", false)) {
				FrozenMember frozen;
				frozen.endpoint = row.at("endpoint").get<room::ConnectionRef>();
				frozen.data = row.at("data").get<SessionProtocol::MemberData>();
				frozen.data.connId = {frozen.endpoint.host, frozen.endpoint.user};
				frozen.data.roomMember = member;
				frozen.data.incarnation = incarnation;
				frozen.incarnation = incarnation;
				roomFrozenMembers[member] = std::move(frozen);
			}
		}
		_recoveryPendingSelected.clear();
		for (const auto& row : value.value("selected_members", json::array())) _recoveryPendingSelected[row.at("member").get<room::MemberId>()] = row.at("table").get<std::uint8_t>();
		_recoveryPendingBattleLoaded = std::move(pendingBattleLoaded);
		_recoveryPendingPunchReady = std::move(pendingPunchReady);
		// Synthetic handles exist only while converting the schema; a passive
		// follower must wait for authenticated RebindMember calls.
		clients.clear(); cidMap.clear(); roomMembers.clear(); roomSelectedTables.clear();
		for (auto& set : _roomBattleLoaded) set.clear(); for (auto& set : _roomPunchReady) set.clear();
		_hasRecoveryProjection = false;
		// A portable checkpoint may contain a committed native preparation. Keep
		// its stable table/generation cancellation record until the new writable
		// owner explicitly commits the teardown.
		if (_preparationCancellationRequested) RememberInterruptedPreparations();
		_passiveTimerClock = 0;
		return true;
	} catch (const std::exception&) { return false; }
}

void SessionServer::BeginRecoveryCandidate() {
	if (!_recovery.Enabled() || _recoveryCandidateReady || _recovery.PendingProposal()) return;
	try {
		_recoveryBaseline = RecoveryCheckpoint();
		_recoveryBaselineBindings.clear();
		for (const auto& row : clients) {
			const auto member = roomMembers.find(row.conn);
			const auto cid = cidMap.find(row.conn);
			if (member != roomMembers.end() && cid != cidMap.end())
				_recoveryBaselineBindings.emplace_back(member->second, row.conn, cid->second,
					row.data.incarnation ? row.data.incarnation : 1);
		}
		_recoveryProjection = _roomAuthority->SnapshotCopy();
		_hasRecoveryProjection = true;
		_recoveryEffects.clear();
		_recoveryLocalEffects.clear();
		_recoveryCandidateOverflow = false;
		_recoveryCandidateReady = true;
	} catch (const std::exception&) {
		_recoveryCandidateReady = false;
	}
}

void SessionServer::FinishRecoveryCandidate() {
	if (!_recovery.Enabled() || !_recoveryCandidateReady) return;
	// A no-op poll must not hold the transport behind a proposal that can never
	// produce an effect.  A timer tick with an outstanding relative result age
	// is still a logical mutation and therefore remains a candidate; the only
	// discarded case here is an exact checkpoint match.
	try {
		if (_recoveryEffects.empty() && RecoveryCheckpoint() == _recoveryBaseline) {
			DropRecoveryCandidate();
			return;
		}
	} catch (...) {
		// Keep the candidate private and fail closed if comparison itself fails.
	}
	// A candidate remains private until the helper commits its exact effect
	// digest. Step() will not consume another poll while this flag is set.
}

void SessionServer::DropRecoveryCandidate() {
	// Used only when the private poll/timer produced no logical mutation. The
	// live authority and native capabilities are already unchanged, so
	// round-tripping the portable checkpoint here would destroy local grants
	// and rebase an otherwise healthy monotonic clock.
	_recovery.Discard();
	_recoveryEffects.clear();
	_recoveryLocalEffects.clear();
	_recoveryCandidateOverflow = false;
	_recoveryCandidateReady = false;
	_recoveryBaseline.clear();
	_recoveryBaselineBindings.clear();
	_hasRecoveryProjection = false;
	_cancellationCandidate = false;
}

bool SessionServer::RestoreRecoveryBaseline() {
	if (!_recoveryCandidateReady || !_recoveryBaseline.is_object()) return true;
	const auto bindings = _recoveryBaselineBindings;
	const bool restored = RestoreRecoveryCheckpoint(_recoveryBaseline);
	const bool rebound = restored && RebindMembers(bindings);
	_recoveryEffects.clear(); _recoveryLocalEffects.clear(); _recoveryCandidateReady = false; _recoveryBaseline.clear();
	_recoveryBaselineBindings.clear();
	_recoveryCandidateOverflow = false;
	_cancellationCandidate = false;
	return restored && rebound;
}

void SessionServer::SetAuthority(std::uint64_t term, std::uint64_t revision, bool writable,
	bool coordinationHealthy) {
	const auto before = _recovery.Authority();
	const bool becameNonWritable = before.term && before.writable && !writable;
	// A passive follower is routinely reported as non-writable while it is
	// still healthy.  Treating that flag alone as a leadership loss would
	// erase a prepared native authority before the committed checkpoint has
	// arrived.  A term transition is the durable loss signal; the helper's
	// higher-term owner then drives any required cancellation through a new
	// candidate.
	const bool lost = before.term != 0 && term != before.term;
	if (lost) {
		_preparationCancellationRequested = true;
		if (!_recoveryCandidateReady && !_recovery.PendingProposal()) RememberInterruptedPreparations();
	}
	if (lost && _recoveryCandidateReady && !RestoreRecoveryBaseline()) _transportFailed = true;
	_recovery.SetAuthority(term, revision, writable);
	_coordinationHealthy = writable || coordinationHealthy;
	if (!_coordinationHealthy) _passiveTimerClock = 0;
	// Same-term follower transitions do not enter the term-loss branch above,
	// but they still suspend result/chat monotonic clocks.  Pause immediately
	// at the authority boundary so a 30-second dispute cannot mature between
	// the state watch and the next passive tick.
	if (becameNonWritable && _roomAuthority && !_roomAuthority->RecoveryPaused())
		_roomAuthority->PauseForRecovery();
	if (lost) CancelPrecommittedGenerations();
}

bool SessionServer::IsInterruptedPreparation(std::uint8_t table, std::uint64_t generation) const {
	return std::any_of(_pendingInterruptedPreparations.begin(), _pendingInterruptedPreparations.end(),
		[&](const InterruptedPreparation& pending) { return pending.table == table && pending.generation == generation; });
}

void SessionServer::RememberInterruptedPreparations() {
	if (!_roomAuthority) return;
	const auto& snapshot = _roomAuthority->SnapshotView();
	for (std::uint8_t table = 0; table < room::TableCount; ++table) {
		const auto* authority = _roomMatchAuthorities[table].get();
		if (!authority || authority->GetPhase() == session::MatchAuthority::Phase::Idle ||
			authority->GetPhase() == session::MatchAuthority::Phase::Started || !authority->Generation()) continue;
		if (snapshot.tables[table].phase != room::TablePhase::Playing ||
			snapshot.tables[table].matchGeneration != authority->Generation()) continue;
		if (!IsInterruptedPreparation(table, authority->Generation()))
			_pendingInterruptedPreparations.push_back({table, authority->Generation()});
	}
}

void SessionServer::CancelPrecommittedGenerations() {
	for (std::uint8_t table = 0; table < room::TableCount; ++table) {
		auto& authority = _roomMatchAuthorities[table];
		// Keep a committed preparation alive until the new owner can journal its
		// game_end.  The stable table/generation record survives the term change;
		// uncommitted candidates have no pending record and can be discarded.
		if (authority && !IsInterruptedPreparation(table, authority->Generation())) authority->CancelPreparation();
	}
	if (_matchAuthority) _matchAuthority->CancelPreparation();
}

void SessionServer::JournalEffect(session::Connection client, const json& payload, bool retainPublicReplay) {
	if (!_recovery.Enabled() || _recoveryFlushing) {
		if (!_transport || !_transport->Send(client, payload.dump())) _transportFailed = true;
		return;
	}
	if (!_recoveryCandidateReady) BeginRecoveryCandidate();
	if (!_recoveryCandidateReady) return;
	room::MemberId member = 0;
	const auto iter = roomMembers.find(client);
	if (iter != roomMembers.end()) member = iter->second;
	if (!member || !_roomAuthority) return; // no recipient path for an unauthenticated handle
	const auto findMember = [&](room::MemberId id) -> const room::Member* {
		for (const auto& value : _roomAuthority->SnapshotView().members) if (value.id == id) return &value;
		return nullptr;
	};
	const auto* roomMember = findMember(member);
	room::ConnectionRef endpoint;
	if (roomMember) {
		endpoint = roomMember->connection;
	} else {
		// Leave/Kick mutates RoomAuthority before its neutral response is
		// journaled. Resolve the recipient from the candidate's stable baseline,
		// never from the now-shortened public roster.
		if (!_recoveryBaseline.is_object() || !_recoveryBaseline.contains("members") ||
			!_recoveryBaseline["members"].is_array()) return;
		const auto baseline = std::find_if(_recoveryBaseline["members"].begin(), _recoveryBaseline["members"].end(),
			[&](const json& row) { return row.value("member", room::MemberId(0)) == member; });
		if (baseline == _recoveryBaseline["members"].end()) return;
		endpoint = baseline->value("endpoint", room::ConnectionRef{});
		if (endpoint.host.empty() || endpoint.user.empty()) return;
	}
	session::EffectEnvelope envelope;
	envelope.sequence = _nextEffectSequence++;
	envelope.roomEpoch = _roomAuthority->SnapshotView().roomEpoch;
	envelope.term = _recovery.Authority().term;
	envelope.revision = _recovery.Authority().revision + 1;
	envelope.generation = payload.value("generation", std::uint64_t(0));
	envelope.recipient = member;
	envelope.endpoint = endpoint;
	envelope.type = payload.value("type", std::string());
	envelope.payloadDigest = session::PayloadDigest(payload);
	const auto type = envelope.type;
	const bool publicReplay = type == "room_snapshot" || type == "room_result" || type == "room_event" ||
		type == "data_update" || type == "game_start" || type == "game_end" || type == "game_peer_end";
	// game_connect remains a private native transition. Pair capabilities and
	// connection setup are never replayed from a replicated payload.
	envelope.privatePayload = !publicReplay || session::ContainsCapabilityField(payload);
	if (!envelope.privatePayload && retainPublicReplay) envelope.publicPayload = payload;
	if (type == "room_snapshot" || type == "data_update") {
		for (std::size_t i = 0; i < _recoveryEffects.size();) {
			if (_recoveryEffects[i].recipient == envelope.recipient && _recoveryEffects[i].type == type) {
				_recoveryEffects.erase(_recoveryEffects.begin() + i);
				_recoveryLocalEffects.erase(_recoveryLocalEffects.begin() + i);
			} else ++i;
		}
	}
	for (const auto& prior : _recoveryEffects) {
		if (prior.recipient == envelope.recipient && prior.revision == envelope.revision && prior.type == envelope.type && prior.payloadDigest == envelope.payloadDigest) return;
	}
	std::vector<session::EffectEnvelope> prospective = _recoveryEffects;
	prospective.push_back(envelope);
	const auto prospectiveBytes = session::ShedOptionalEffectPayloads(prospective, session::MaxEffectJournalBytes);
	if (prospective.size() > session::MaxEffectJournalEntries || prospectiveBytes > session::MaxEffectJournalBytes) {
		_recoveryCandidateOverflow = true;
		return;
	}
	// Existing local payloads remain intact; only their replicated envelope's
	// optional replay copy may have been shed by the whole-candidate pass.
	for (std::size_t i = 0; i < _recoveryLocalEffects.size(); ++i)
		_recoveryLocalEffects[i].envelope = prospective[i];
	envelope = prospective.back();
	_recoveryEffects = std::move(prospective);
	_recoveryLocalEffects.push_back({envelope, payload, client});
}

bool SessionServer::ValidateEffectRecipient(const session::EffectEnvelope& effect, session::Connection candidate, session::Connection& local) const {
	if (!_roomAuthority || !effect.recipient || effect.endpoint.host.empty() || effect.endpoint.user.empty()) return false;
	const auto currentEpoch = _roomAuthority->SnapshotView().roomEpoch;
	if (effect.roomEpoch && effect.roomEpoch != currentEpoch) return false;
	const auto member = std::find_if(_roomAuthority->SnapshotView().members.begin(), _roomAuthority->SnapshotView().members.end(),
		[&](const room::Member& value) { return value.id == effect.recipient && value.connection.host == effect.endpoint.host && value.connection.user == effect.endpoint.user; });
	if (member != _roomAuthority->SnapshotView().members.end()) {
		for (const auto& mapping : roomMembers) {
			if (mapping.second != effect.recipient) continue;
			const auto cid = cidMap.find(mapping.first);
			if (cid != cidMap.end() && cid->second.host == effect.endpoint.host && cid->second.user == effect.endpoint.user) { local = mapping.first; return true; }
		}
	}
	// A Leave or Kick response is journaled before the member is removed from
	// the live roster.  Preserve that one response using the stable baseline
	// row and the original local handle; never resolve a departed member from
	// the current room snapshot or by a newly rebound numeric handle.
	if (!_recoveryBaseline.is_object() || !_recoveryBaseline.contains("members") ||
		!_recoveryBaseline["members"].is_array()) return false;
	bool baselineMatch = false;
	for (const auto& row : _recoveryBaseline["members"]) {
		if (row.value("member", room::MemberId(0)) != effect.recipient) continue;
		const auto endpoint = row.value("endpoint", room::ConnectionRef{});
		baselineMatch = endpoint == effect.endpoint;
		break;
	}
	if (!baselineMatch || !candidate) return false;
	const auto currentCid = cidMap.find(candidate);
	if (currentCid != cidMap.end()) {
		if (currentCid->second == SessionProtocol::ConnectionID{effect.endpoint.host, effect.endpoint.user}) {
			local = candidate; return true;
		}
		return false; // the numeric handle has already been rebound to another peer
	}
	const auto currentClient = std::find_if(clients.begin(), clients.end(),
		[&](const SessionMember& row) { return row.conn == candidate; });
	if (currentClient != clients.end()) return false;
	local = candidate;
	return true;
}

bool SessionServer::ProposeCheckpoint(std::uint64_t request, std::uint64_t term, std::uint64_t baseRevision, const json& checkpoint) {
	if (!_recoveryCandidateReady || !_recovery.Writable() || term != _recovery.Authority().term || baseRevision != _recovery.Authority().revision || request == 0) return false;
	if (_recoveryCandidateOverflow) return false;
	json local;
	try { local = RecoveryCheckpoint(); } catch (...) { return false; }
	// Root may attach its own metadata, but state must be byte-for-byte the
	// candidate produced by this owner before it can be committed.
	if (!checkpoint.is_null() && checkpoint != local) return false;
	session::SessionProposal proposal;
	proposal.request = request; proposal.term = term; proposal.baseRevision = baseRevision;
	proposal.checkpoint = local; proposal.effects = _recoveryEffects; proposal.effectsDigest = session::EffectsDigest(proposal.effects);
	if (json(proposal).dump().size() > kRecoveryProposalBytes) return false;
	return _recovery.Prepare(std::move(proposal));
}

bool SessionServer::ApplyCommit(std::uint64_t request, std::uint64_t term, std::uint64_t revision,
	const json& committedCheckpoint, const session::EffectDigest& effectsDigest) {
	const auto pending = _recovery.PendingProposal();
	if (!pending || !_recovery.Writable() || term != _recovery.Authority().term || request != pending->request ||
		effectsDigest != pending->effectsDigest || revision <= _recovery.Authority().revision) return false;
	if (!committedCheckpoint.is_null() && committedCheckpoint != pending->checkpoint) return false;
	if (!_recovery.Commit(request, term, revision, effectsDigest)) return false;
	_recoveryFlushing = true;
	for (const auto& effect : _recoveryLocalEffects) {
		session::Connection local = effect.local;
		if (!ValidateEffectRecipient(effect.envelope, effect.local, local)) continue; // departed member; helper owns the removal decision
		// The client still consumes the original protocol object.  The commit
		// envelope is additive and gives the recipient enough immutable material
		// to reject a stale/mutated replay before dispatching the payload.
		json wire = effect.payload;
		// Keep this byte-for-byte aligned with EffectEnvelope::to_json so the
		// recipient can parse one token shape from both a live effect and a
		// checkpoint journal.
		wire["_commit"] = session::EffectCommitToken(effect.envelope);
		if (!_transport || !_transport->Send(local, wire.dump())) _transportFailed = true;
	}
	_recoveryFlushing = false;
	for (const auto& effect : _recoveryEffects) _committedEffectHistory.push_back(effect);
	// Keep lifecycle/final-result effects in order while compacting only
	// supersedable projections. The same helper is used by the root recovery
	// bridge when it merges a checkpoint journal with a committed proposal.
	session::CompactEffectJournal(_committedEffectHistory);
	_recoveryEffects.clear(); _recoveryLocalEffects.clear(); _recoveryCandidateReady = false; _recoveryBaseline.clear(); _recoveryBaselineBindings.clear();
	_recoveryCandidateOverflow = false;
	_hasRecoveryProjection = false;
	if (_cancellationCandidate) {
		// ApplyCommit has already advanced the authoritative revision. The
		// teardown records are now durable and must not be repeated on the next
		// handoff.
		_pendingInterruptedPreparations.clear();
		_preparationCancellationRequested = false;
		_cancellationCandidate = false;
	}
	return !_transportFailed;
}

void SessionServer::DiscardProposal() {
	_recovery.Discard();
	if (!RestoreRecoveryBaseline()) _transportFailed = true;
}

bool SessionServer::CancelInterruptedPreparations() {
	if (!_roomAuthority || !_recovery.Enabled() || !_recovery.Writable() ||
		_recoveryCandidateReady || _recovery.PendingProposal()) return false;
	if (!_preparationCancellationRequested) return true;
	if (_pendingInterruptedPreparations.empty()) {
		_preparationCancellationRequested = false;
		return true;
	}

	struct PendingTable {
		std::uint8_t table = 0;
		std::uint64_t generation = 0;
		session::MatchAuthority* authority = nullptr;
		std::vector<room::MemberId> recipients;
	};
	std::vector<PendingTable> active;
	const auto snapshot = _roomAuthority->SnapshotCopy();
	std::vector<InterruptedPreparation> retained;
	for (const auto& pending : _pendingInterruptedPreparations) {
		if (pending.table >= room::TableCount) continue;
		auto* authority = _roomMatchAuthorities[pending.table].get();
		const auto& table = snapshot.tables[pending.table];
		if (!authority || authority->GetPhase() == session::MatchAuthority::Phase::Idle ||
			table.phase != room::TablePhase::Playing || table.matchGeneration != pending.generation) {
			// The imported checkpoint may already contain the committed teardown,
			// or a later generation may have replaced this one.
			continue;
		}
		if (authority->GetPhase() == session::MatchAuthority::Phase::Started) {
			// Started is a durable gameplay state. Never tear it down as part of
			// preparation recovery.
			continue;
		}
		PendingTable item;
		item.table = pending.table; item.generation = pending.generation; item.authority = authority;
		for (const auto member : {table.p1, table.p2}) if (member &&
			std::find(item.recipients.begin(), item.recipients.end(), member) == item.recipients.end()) item.recipients.push_back(member);
		for (const auto member : table.spectators) if (member &&
			std::find(item.recipients.begin(), item.recipients.end(), member) == item.recipients.end()) item.recipients.push_back(member);
		active.push_back(std::move(item));
		retained.push_back(pending);
	}
	_pendingInterruptedPreparations = std::move(retained);
	if (active.empty()) return true;
	BeginRecoveryCandidate();
	if (!_recoveryCandidateReady) return false;
	std::vector<room::Event> events;
	for (const auto& item : active) {
		// MatchAuthority::End sends the private native teardown through the
		// candidate journal. It cannot touch a socket while the candidate is
		// uncommitted.
		if (!item.authority->End(MatchSender())) return false;
		const auto ended = _roomAuthority->EndMatch(item.table, item.generation, room::MatchResult::Abort);
		if (!ended.accepted) return false;
		_roomBattleLoaded[item.table].clear();
		_roomPunchReady[item.table].clear();
		_roomMatchData[item.table].Clear();
		events.insert(events.end(), ended.events.begin(), ended.events.end());
		// Pair the teardown with an acknowledged room result before the regular
		// snapshot/event burst. actionId=0 is the legacy synthetic recovery form.
		for (const auto member : item.recipients) {
			for (const auto& mapping : roomMembers) {
				if (mapping.second != member) continue;
				SessionProtocol::RoomResultMessage response;
				response.result.accepted = true;
				response.result.snapshot = _roomAuthority->SnapshotFor(member);
				Respond(mapping.first, json(response));
				break;
			}
		}
	}
	BroadcastRoomState(events);
	_cancellationCandidate = true;
	return true;
}

bool SessionServer::RebindMembers(const std::vector<StableRebind>& bindings) {
	if (!_roomAuthority) return false;
	const auto& snapshot = _roomAuthority->SnapshotView();
	if (bindings.empty()) return snapshot.members.empty();
	if (bindings.size() != snapshot.members.size()) return false;
	std::set<room::MemberId> members;
	std::set<session::Connection> handles;
	std::set<std::pair<std::string, std::string>> endpoints;
	const auto findRoomMember = [&](room::MemberId id) -> const room::Member* {
		for (const auto& value : snapshot.members) if (value.id == id) return &value;
		return nullptr;
	};
	for (const auto& binding : bindings) {
		const auto member = std::get<0>(binding);
		const auto local = std::get<1>(binding);
		const auto& cid = std::get<2>(binding);
		const auto incarnation = std::get<3>(binding);
		const auto* roomMember = findRoomMember(member);
		if (!roomMember || !local || !incarnation || roomMember->connection.host != cid.host ||
			roomMember->connection.user != cid.user || !members.insert(member).second ||
			!handles.insert(local).second || !endpoints.emplace(cid.host, cid.user).second) return false;
		const auto priorIncarnation = roomIncarnations.find(member);
		if (priorIncarnation != roomIncarnations.end() && priorIncarnation->second && priorIncarnation->second != incarnation) return false;
	}
	if (members.size() != snapshot.members.size()) return false;
	for (const auto& row : clients) {
		const auto member = roomMembers.find(row.conn);
		if (member == roomMembers.end() || !members.count(member->second)) return false;
	}

	// Capture the current native mapping so a failed authority update can be
	// rolled back without regenerating private capabilities or changing phase.
	std::vector<json> oldPortable;
	for (const auto& authority : _roomMatchAuthorities) oldPortable.push_back(authority ? authority->PortableCheckpoint() : json(nullptr));
	const json oldLegacy = _matchAuthority ? _matchAuthority->PortableCheckpoint() : json(nullptr);
	const auto oldHandleFor = [&](const SessionProtocol::ConnectionID& endpoint) -> session::Connection {
		for (const auto& pair : cidMap) if (pair.second == endpoint) return pair.first;
		for (const auto& member : snapshot.members) if (member.connection.host == endpoint.host && member.connection.user == endpoint.user) return static_cast<session::Connection>(member.id);
		return 0;
	};
	std::vector<session::MatchAuthority::RebindEntry> fullMapping;
	for (const auto& binding : bindings) fullMapping.push_back({std::get<2>(binding), std::get<1>(binding)});
	const auto rollbackAuthorities = [&]() {
		const auto oldResolver = [&](const SessionProtocol::ConnectionID& endpoint) { return oldHandleFor(endpoint); };
		for (std::size_t i = 0; i < _roomMatchAuthorities.size(); ++i) {
			if (_roomMatchAuthorities[i] && i < oldPortable.size()) _roomMatchAuthorities[i]->RestorePortableCheckpoint(oldPortable[i], oldResolver);
		}
		if (_matchAuthority && !oldLegacy.is_null()) _matchAuthority->RestorePortableCheckpoint(oldLegacy, oldResolver);
	};
	for (auto& authoritySlot : _roomMatchAuthorities) {
		auto* authority = authoritySlot.get();
		if (!authority || authority->GetPhase() == session::MatchAuthority::Phase::Idle) continue;
		if (!authority->RebindConnections(fullMapping)) { rollbackAuthorities(); return false; }
	}
	if (_matchAuthority && _matchAuthority->GetPhase() != session::MatchAuthority::Phase::Idle &&
		!_matchAuthority->RebindConnections(fullMapping)) {
		rollbackAuthorities(); return false;
	}

	std::map<room::MemberId, SessionMember> oldRows;
	std::map<room::MemberId, session::Connection> oldConnections;
	for (const auto& row : clients) {
		const auto member = roomMembers.find(row.conn);
		if (member != roomMembers.end()) {
			oldRows[member->second] = row;
			oldConnections[member->second] = row.conn;
		}
	}
	std::map<session::Connection, SessionProtocol::ConnectionID> newCids;
	std::map<session::Connection, room::MemberId> newMembers;
	std::map<session::Connection, std::uint8_t> newSelected;
	std::vector<SessionMember> newClients;
	std::array<std::set<session::Connection>, room::TableCount> newBattleLoaded{};
	std::array<std::set<session::Connection>, room::TableCount> newPunchReady{};
	for (const auto& binding : bindings) {
		const auto member = std::get<0>(binding);
		const auto local = std::get<1>(binding);
		const auto& cid = std::get<2>(binding);
		const auto incarnation = std::get<3>(binding);
		SessionMember row{};
		const auto prior = oldRows.find(member);
		if (prior != oldRows.end()) row = prior->second;
		const auto* roomMember = findRoomMember(member);
		row.conn = local; row.data.connId = cid; row.data.name = roomMember->name;
		row.data.roomMember = member; row.data.authenticatedEndpoint = roomPeerIdentities.count(member) ? roomPeerIdentities.at(member) : cid.user;
		row.data.incarnation = incarnation;
		newClients.push_back(row); newCids[local] = cid; newMembers[local] = member;
		const auto priorSelected = prior == oldRows.end() ? roomSelectedTables.end() : roomSelectedTables.find(prior->second.conn);
		newSelected[local] = priorSelected != roomSelectedTables.end() ? priorSelected->second :
			(_recoveryPendingSelected.count(member) ? _recoveryPendingSelected.at(member) : 0);
		for (std::size_t table = 0; table < room::TableCount; ++table) {
			const auto oldConnection = oldConnections.find(member);
			if ((oldConnection != oldConnections.end() && _roomBattleLoaded[table].count(oldConnection->second)) || _recoveryPendingBattleLoaded[table].count(member)) newBattleLoaded[table].insert(local);
			if ((oldConnection != oldConnections.end() && _roomPunchReady[table].count(oldConnection->second)) || _recoveryPendingPunchReady[table].count(member)) newPunchReady[table].insert(local);
		}
		roomIncarnations[member] = incarnation;
		_roomAuthority->SetMemberIncarnation(member, incarnation);
	}
	clients = std::move(newClients); cidMap = std::move(newCids); roomMembers = std::move(newMembers);
	roomSelectedTables = std::move(newSelected); _roomBattleLoaded = std::move(newBattleLoaded); _roomPunchReady = std::move(newPunchReady);
	for (const auto& binding : bindings) {
		const auto member = std::get<0>(binding);
		_recoveryPendingSelected.erase(member);
		for (auto& set : _recoveryPendingBattleLoaded) set.erase(member);
		for (auto& set : _recoveryPendingPunchReady) set.erase(member);
	}
	return true;
}

bool SessionServer::RebindMember(room::MemberId member, session::Connection local,
	const SessionProtocol::ConnectionID& cid, std::uint64_t incarnation) {
	if (!member || !local || !incarnation || !_roomAuthority) return false;
	const auto iter = std::find_if(_roomAuthority->SnapshotView().members.begin(), _roomAuthority->SnapshotView().members.end(),
		[&](const room::Member& value) { return value.id == member; });
	if (iter == _roomAuthority->SnapshotView().members.end() || iter->connection.host != cid.host || iter->connection.user != cid.user) return false;
	if (roomMembers.count(local) || cidMap.count(local)) return false;
	roomMembers[local] = member; cidMap[local] = cid;
	const auto selected = _recoveryPendingSelected.find(member);
	roomSelectedTables[local] = selected == _recoveryPendingSelected.end() ? 0 : selected->second;
	roomIncarnations[member] = incarnation;
	_roomAuthority->SetMemberIncarnation(member, incarnation);
	SessionMember row{}; row.conn = local; row.data.connId = cid; row.data.name = iter->name; row.data.roomMember = member;
	row.data.authenticatedEndpoint = roomPeerIdentities.count(member) ? roomPeerIdentities.at(member) : cid.user; row.data.incarnation = incarnation;
	clients.push_back(row);
	for (std::size_t table = 0; table < room::TableCount; ++table) {
		if (_recoveryPendingBattleLoaded[table].erase(member)) _roomBattleLoaded[table].insert(local);
		if (_recoveryPendingPunchReady[table].erase(member)) _roomPunchReady[table].insert(local);
	}
	_recoveryPendingSelected.erase(member);
	for (auto& authority : _roomMatchAuthorities) if (authority) authority->RebindConnection(cid, local);
	if (_matchAuthority) _matchAuthority->RebindConnection(cid, local);
	return true;
}

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
	if (!add(table.p1) || !add(table.p2)) return false;
	for (const auto spectator : table.spectators) if (!add(spectator)) return false;
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
	if (_recovery.Enabled() && _recoveryCandidateReady && events.empty() && _recoveryEffects.empty()) {
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
	for (const auto id : table.spectators) {
		const auto* roomMember = dataFor(id);
		if (!roomMember) continue;
		SessionProtocol::MemberData data;
		data.connId = cidFor(*roomMember);
		data.name = roomMember->name;
		data.ip.clear(); data.port = 0; data.flags = 0;
		update.lobbyData.members.push_back(data);
	}
	update.matchData = _roomMatchData[tableId];
	update.matchData.inputDelay[0] = table.inputDelay[0];
	update.matchData.inputDelay[1] = table.inputDelay[1];
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

void SessionServer::BroadcastRoomState(const std::vector<room::Event>& events) {
	if (!_roomAuthority) return;
	for (const auto& client : clients) {
		const auto member = roomMembers.find(client.conn);
		if (member == roomMembers.end()) continue;
		SessionProtocol::RoomSnapshotMessage snapshot;
		snapshot.snapshot = _roomAuthority->SnapshotFor(member->second);
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

void SessionServer::AddConnection(session::Connection connection) {
	if (!_transport || !_transport->Attach(connection)) _transportFailed = true;
}

int SessionServer::Listen(uint16_t port) {
	return _transport && _transport->Listen(port) ? 0 : -1;
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
				SessionProtocol::SessionHelloMsg hello;
				try { msg.get_to(hello); }
				catch (const std::exception&) { spdlog::warn("Server: malformed session hello"); continue; }
				SessionProtocol::SessionHelloResp cidMsg;
				cidMsg.cid.host = _identity;
				cidMsg.cid.user = std::to_string(conn);
				// Native room CIDs must survive transport-handle churn.  The
				// helper-backed authorization resolver already has the fresh
				// authenticated endpoint for this connection, while the numeric
				// handle is only a local socket slot and can be reused after a
				// takeover.
				if (_roomAuthority && _matchAuthorizationConfigured && _matchAuthorizationIdentity) {
					const auto stable = _matchAuthorizationIdentity(conn);
					if (!stable.empty()) cidMsg.cid.user = stable;
				}
				cidMap[conn] = cidMsg.cid;
				SessionProtocol::JoinResult admissionResult = SessionProtocol::JOIN_OK;
				bool admitted = false;
				if (!hello.admission.is_null() && !hello.admission.empty()) {
					try {
						SessionProtocol::SessionJoinRequest admission = hello.admission.get<SessionProtocol::SessionJoinRequest>();
						if ((_roomAuthority && (!admission.customRooms || admission.roomProtocol != room::ProtocolVersion)) ||
							(!_roomAuthority && admission.customRooms)) admissionResult = SessionProtocol::JR_REQUEST_INVALID;
						else admissionResult = RegisterToWait(conn, admission.port, admission.sidecarHash, admission.username,
							incoming.peerAddress, cidMsg.cid, admission.mainFighter);
						admitted = admissionResult == SessionProtocol::JOIN_OK;
					} catch (const std::exception&) { admissionResult = SessionProtocol::JR_REQUEST_INVALID; }
				}
				if (admitted && _roomAuthority) {
					cidMsg.roomMember = roomMembers[conn];
					cidMsg.authenticatedEndpoint = roomPeerIdentities[cidMsg.roomMember];
					cidMsg.incarnation = roomIncarnations[cidMsg.roomMember];
					// The bootstrap hello is the complete custom-room admission.
					// Journal its initial room projection in this same candidate so
					// the client never needs the legacy join_req round trip.
					_dataDirty = true;
				}
				// A bare hello and an admission rejection are transport handshakes,
				// not room effects. An admitted hello is journaled with the room
				// member so it cannot expose a provisional roster before commit.
				if (admitted) Respond(conn, json(cidMsg));
				else if (!_transport || !_transport->Send(conn, json(cidMsg).dump())) _transportFailed = true;
				if (!admitted && !hello.admission.is_null() && !hello.admission.empty()) {
					SessionProtocol::SessionJoinReject reject; reject.result = admissionResult;
					if (!_transport || !_transport->Send(conn, json(reject).dump())) _transportFailed = true;
				}
			}
			else {
				spdlog::warn("Server: got unrecognized message type: {}", (int)type);
			}
		}
		else {
			SessionProtocol::ConnectionID cid = cidMap[conn];
			const auto name = msg.value("type", std::string());
			if (type == SessionProtocol::MT_ROOM_ACTION && _roomAuthority) {
				SessionProtocol::RoomActionMessage actionMessage;
				try { msg.get_to(actionMessage); }
				catch (const std::exception&) {
					SessionProtocol::RoomResultMessage invalid;
					invalid.result.accepted = false;
					invalid.result.reason = room::RejectReason::StaleRoom;
					Respond(conn, json(invalid));
					continue;
				}
				const auto roomMember = roomMembers.find(conn);
				if (roomMember == roomMembers.end()) continue;
				const auto priorSnapshot = _roomAuthority->SnapshotCopy();
				if (actionMessage.action.kind == room::ActionKind::Leave) CaptureFrozenMember(roomMember->second, priorSnapshot);
				else if (actionMessage.action.kind == room::ActionKind::Kick) CaptureFrozenMember(actionMessage.action.target, priorSnapshot);
				auto result = _roomAuthority->Apply(roomMember->second, actionMessage.action);
				std::vector<room::Event> events = result.events;
				const bool duplicateTerminalAcknowledgment = result.accepted &&
					actionMessage.action.kind == room::ActionKind::AcknowledgeTerminal &&
					result.snapshot.revision == priorSnapshot.revision && events.empty();
				if (result.accepted && (actionMessage.action.kind == room::ActionKind::Leave ||
					actionMessage.action.kind == room::ActionKind::Unwatch ||
					actionMessage.action.kind == room::ActionKind::Watch || actionMessage.action.kind == room::ActionKind::Kick)) {
					const room::MemberId departedMember = actionMessage.action.kind == room::ActionKind::Kick
						? actionMessage.action.target : roomMember->second;
					const auto connectionFor = [&](room::MemberId id) -> session::Connection {
						for (const auto& mapping : roomMembers) if (mapping.second == id) return mapping.first;
						return 0;
					};
					const auto oldWasInTable = [&](const room::Table& table) {
						return table.p1 == departedMember || table.p2 == departedMember ||
							std::find(table.spectators.begin(), table.spectators.end(), departedMember) != table.spectators.end() ||
							std::find(table.watchingNext.begin(), table.watchingNext.end(), departedMember) != table.watchingNext.end();
					};
					for (std::uint8_t tableId = 0; tableId < room::TableCount; ++tableId) {
						const auto& oldTable = priorSnapshot.tables[tableId];
						if (!oldWasInTable(oldTable) || (actionMessage.action.kind == room::ActionKind::Watch && tableId == actionMessage.action.table)) continue;
						auto* authority = RoomMatchAuthority(tableId);
						if (!authority || authority->GetPhase() == session::MatchAuthority::Phase::Idle) continue;
						const auto departing = connectionFor(departedMember);
						if (!departing || !authority->MemberDeparted(departing, MatchSender())) { _transportFailed = true; continue; }
						// A pre-start link or a fighter departure aborts the room
						// generation so the next grant is built from a fresh roster.
						const bool fighter = oldTable.p1 == departedMember || oldTable.p2 == departedMember;
						if (fighter || authority->GetPhase() == session::MatchAuthority::Phase::Idle) {
							const auto current = _roomAuthority->SnapshotView().tables[tableId];
							if (current.phase == room::TablePhase::Playing || current.phase == room::TablePhase::Paused) {
								auto aborted = _roomAuthority->EndMatch(tableId, current.matchGeneration, room::MatchResult::Abort);
								if (aborted.accepted) events.insert(events.end(), aborted.events.begin(), aborted.events.end());
							}
						}
					}
				}
				for (const auto& event : result.events) {
					if (event.kind == room::Event::Kind::MatchReady) {
						const auto& table = _roomAuthority->SnapshotView().tables[event.table];
						auto started = _roomAuthority->BeginMatch(event.table, table.p1, table.p2);
						if (started.accepted) {
							events.insert(events.end(), started.events.begin(), started.events.end());
							for (const auto& startedEvent : started.events) {
								if (startedEvent.kind == room::Event::Kind::MatchStarted &&
									!BeginAuthorizedTable(startedEvent.table, startedEvent.matchGeneration)) {
									_transportFailed = true;
								}
							}
						}
					}
					else if (event.kind == room::Event::Kind::MatchEnded) {
						auto* authority = RoomMatchAuthority(event.table);
						bool nativeEndSent = false;
						if (authority && authority->Generation() == event.matchGeneration &&
							authority->GetPhase() != session::MatchAuthority::Phase::Idle) {
							nativeEndSent = true;
							if (!authority->End(MatchSender())) _transportFailed = true;
						}
						// A terminal receipt can outlive MatchAuthority and the generic
						// effect journal. Replay the exact fighter generation directly
						// from the durable room ledger when native teardown was already
						// retired, so Ending cannot wait forever for an evicted game_end.
						if (event.terminalReplay && !nativeEndSent && _roomAuthority) {
							bool sent = true;
							for (const auto member : _roomAuthority->TerminalMembers(event.table, event.matchGeneration)) {
								for (const auto& mapping : roomMembers) {
									if (mapping.second != member) continue;
									sent = MatchSender()(mapping.first, json{{"type", "game_end"}, {"generation", event.matchGeneration}}) && sent;
									break;
								}
							}
							if (!sent) _transportFailed = true;
						}
					}
					else if (event.kind == room::Event::Kind::RoomClosed) {
						for (auto& authority : _roomMatchAuthorities) {
							if (authority && !authority->End(MatchSender())) _transportFailed = true;
						}
					}
					if (event.kind == room::Event::Kind::MemberRemoved && event.member != roomMember->second) {
						if (actionMessage.action.kind == room::ActionKind::Kick) {
							auto identity = roomPeerIdentities.find(event.member);
							if (identity != roomPeerIdentities.end()) roomBannedIdentities.insert(identity->second);
						}
						for (auto kicked = roomMembers.begin(); kicked != roomMembers.end(); ++kicked) {
							if (kicked->second == event.member) {
								const auto kickedConnection = kicked->first;
								if (actionMessage.action.kind == room::ActionKind::Kick) {
									SessionProtocol::RoomResultMessage kickedResponse;
									kickedResponse.result.accepted = false;
									kickedResponse.result.reason = room::RejectReason::MemberKicked;
									kickedResponse.result.snapshot = _roomAuthority->SnapshotCopy();
									kickedResponse.result.snapshot.localMember = 0;
									Respond(kickedConnection, json(kickedResponse));
								}
								roomSelectedTables.erase(kicked->first);
								roomPeerIdentities.erase(event.member);
								roomMembers.erase(kicked);
								cidMap.erase(kickedConnection);
								clients.erase(std::remove_if(clients.begin(), clients.end(), [&](const SessionMember& client) {
									return client.conn == kickedConnection;
								}), clients.end());
								for (auto& loaded : _roomBattleLoaded) loaded.erase(kickedConnection);
								for (auto& punch : _roomPunchReady) punch.erase(kickedConnection);
								break;
							}
						}
					}
				}
				SessionProtocol::RoomResultMessage response;
				response.result = result;
				response.actionId = actionMessage.action.actionId;
				response.result.snapshot = _roomAuthority->SnapshotFor(roomMember->second);
				if (result.accepted) {
					for (const auto& local : response.result.snapshot.members) {
						if (local.id == roomMember->second && local.table >= 0 && local.table < static_cast<std::int8_t>(room::TableCount)) {
							roomSelectedTables[conn] = static_cast<std::uint8_t>(local.table);
						}
					}
				}
				// ACK clients retain this exact action identity until an accepted
				// response. Commit the full local reply and its stable digest, while
				// leaving the optional successor replay copy out of the bounded journal;
				// a successor answers the stable retry from the receipt/tombstone.
				Respond(conn, json(response), actionMessage.action.kind != room::ActionKind::AcknowledgeTerminal);
				if (result.accepted && actionMessage.action.kind == room::ActionKind::Leave) {
					const auto leavingConnection = conn;
					const auto leavingMember = roomMember->second;
					roomPeerIdentities.erase(leavingMember);
					roomSelectedTables.erase(conn);
					roomMembers.erase(roomMember);
					cidMap.erase(leavingConnection);
					clients.erase(std::remove_if(clients.begin(), clients.end(), [&](const SessionMember& client) {
						return client.conn == leavingConnection;
					}), clients.end());
					for (auto& loaded : _roomBattleLoaded) loaded.erase(leavingConnection);
					for (auto& punch : _roomPunchReady) punch.erase(leavingConnection);
				}
				if (result.accepted) {
					if (actionMessage.action.table < room::TableCount) {
						const auto& before = priorSnapshot.tables[actionMessage.action.table];
						const auto& after = result.snapshot.tables[actionMessage.action.table];
						if (before.p1 != after.p1 || before.p2 != after.p2) _roomMatchData[actionMessage.action.table].Clear();
					}
					// An exact receipt/tombstone retry still commits its room_result so a
					// lost reply can complete, but it did not change public room state.
					// Leave a pre-existing dirty flag untouched and avoid another full
					// snapshot/projection burst for every member.
					if (!duplicateTerminalAcknowledgment) _dataDirty = true;
					// Coalesce all room actions observed in this poll into one
					// snapshot/projection burst. Each action still receives its own
					// result, but 16-member rooms must not enqueue one full snapshot
					// per participant per action.
					deferredRoomEvents.insert(deferredRoomEvents.end(), events.begin(), events.end());
				}
				continue;
			}
			if (name == "game_prepared" || name == "game_ready") {
				try {
					if (_roomAuthority) {
						const auto generation = msg.value("generation", std::uint64_t(0));
						const auto table = RoomTableForGeneration(generation);
						auto* authority = RoomMatchAuthority(table);
						if (authority && !authority->Acknowledge(conn, msg, MatchSender())) _transportFailed = true;
					} else if (_matchAuthority && !_matchAuthority->Acknowledge(conn, msg, MatchSender())) {
						_transportFailed = true;
					}
				} catch (const json::exception&) { /* Malformed acknowledgment has no authority. */ }
				continue;
			}
			if (type == SessionProtocol::MT_FORWARD) {
				SessionProtocol::ForwardMessage fwdMsg;
				try {
					msg.get_to(fwdMsg);
				}
				catch (const json::exception&) {
					spdlog::debug("Server: could not deserialize forwarding message");
					continue;
				}

				// If this is a connection ID managed by this server, we can apply
				// additional security- messages with this source address should
				// only be coming from the connection that the address is assigned
				// to.
				if (!(fwdMsg.src == cid)) {
					spdlog::debug("Server: dropping fraudulent forwarding source");
					continue;
				}
				const auto sourceTable = _roomAuthority ? RoomTableFor(conn) : static_cast<std::uint8_t>(room::TableCount);
				if (_roomAuthority && (!RoomMatchAuthority(sourceTable) || RoomMatchAuthority(sourceTable)->GetPhase() == session::MatchAuthority::Phase::Idle ||
					!IsRoomTableParticipant(conn, sourceTable))) continue;

				if (fwdMsg.dest.host != _identity) {
					spdlog::info("Server: cannot forward to nonlocal identity {}@{}, clustering not yet implemented", fwdMsg.dest.user, fwdMsg.dest.host);
					continue;
				}

				// Check that the destination is in fact the lobby
				auto clientIter = clients.begin();
				for (; clientIter != clients.end(); clientIter++) {
					if (clientIter->data.connId == fwdMsg.dest) {
						break;
					}
				}
				if (clientIter != clients.end() && (!_roomAuthority || IsRoomTableParticipant(clientIter->conn, sourceTable))) {
							Respond(clientIter->conn, msg);
				}
				else {
					spdlog::debug("Server: Could not forward to {}@{}: not in known lobby", fwdMsg.dest.user, fwdMsg.dest.host);
				}
			}
			else if (type == SessionProtocol::MT_SESSION_JOINREQ) {
				SessionProtocol::SessionJoinRequest request;
				const auto main=msg.find("mainFighter");
				const bool invalidMain=main!=msg.end()&&(!main->is_number_integer()||
					(main->is_number_unsigned()?main->get<std::uint64_t>()>43:
					main->get<std::int64_t>() < -1 || main->get<std::int64_t>() > 43));
				if(invalidMain) {
					SessionProtocol::SessionJoinReject reject;reject.result=SessionProtocol::JR_REQUEST_INVALID;
					Respond(conn,json(reject));continue;
				}
				try {
					msg.get_to(request);
				}
				catch (const json::exception&) {
					spdlog::info("Server: could not deserialize join request");
					SessionProtocol::SessionJoinReject reject;
					reject.result = SessionProtocol::JR_REQUEST_INVALID;
					json rejectMsg = reject;
					Respond(conn, rejectMsg);
					continue;
				}
				if ((_roomAuthority && (!request.customRooms || request.roomProtocol != room::ProtocolVersion)) ||
					(!_roomAuthority && request.customRooms)) {
					SessionProtocol::SessionJoinReject reject;
					reject.result = SessionProtocol::JR_REQUEST_INVALID;
					Respond(conn, json(reject));
					continue;
				}

				SessionProtocol::JoinResult joinResult = RegisterToWait(conn, request.port, request.sidecarHash, request.username, incoming.peerAddress, cid, request.mainFighter);
				if (joinResult != SessionProtocol::JOIN_OK) {
					spdlog::info("Server: rejecting registration for reason {}", (int)joinResult);
					SessionProtocol::SessionJoinReject reject;
					reject.result = joinResult;
					json rejectMsg = reject;
					Respond(conn, rejectMsg);
					continue;
				}

				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETCHARA) {
				const std::uint8_t tableId = RoomTableFor(conn);
                if(_roomAuthority&&tableId>=room::TableCount)continue;
				if ((!_roomAuthority && _matchAuthority && _matchAuthority->GetPhase() != session::MatchAuthority::Phase::Idle) ||
					(_roomAuthority && RoomMatchAuthority(tableId) && RoomMatchAuthority(tableId)->GetPhase() != session::MatchAuthority::Phase::Idle)) continue;
				const auto table = _roomAuthority ? _roomAuthority->SnapshotView().tables[tableId] : room::Table{};
				int side = _roomAuthority ? RoomSideFor(conn, tableId) : -1;
				if (!_roomAuthority) for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) { side = i; break; }
				}
				if (side == -1) {
					spdlog::info("Server: sender {} tried to set chara, but is not playing", conn);
					continue;
				}

				SessionProtocol::PreBattleSetChara request;
				try {
					msg.get_to(request);
				}
				catch (const json::exception&) {
					spdlog::info("Server: could not deserialize SetConditionsRequest");
					continue;
				}
				auto& matchData = _roomAuthority ? _roomMatchData[tableId] : _matchData;
				if ((_roomAuthority ? table.ready[side] : matchData.readyMessageNum[side] != -1) ||
					!selection::Valid(selection::FromNative(request.chara), _roomAuthority ? table.rules.editionSelect : _lobbyData.editionSelect)) {
					spdlog::info("Server: rejected unavailable or locked character selection from {}", conn);
					continue;
				}
				matchData.chara[side] = request.chara;
                if(_roomAuthority&&_roomAuthority->SetMemberFighter(side==0?table.p1:table.p2,request.chara.charaID))BroadcastRoomState({});
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETENV) {
				const std::uint8_t tableId = RoomTableFor(conn);
				int side = _roomAuthority ? RoomSideFor(conn, tableId) : -1;
				if (!_roomAuthority) for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) { side = i; break; }
				}
				if (side != 0) {
					spdlog::info("Server: sender {} tried to set env, but is not P1", conn);
					continue;
				}
				SessionProtocol::PreBattleSetEnv request;
				try {
					msg.get_to(request);
				}
				catch (const json::exception&) {
					spdlog::info("Server: could not deserialize SetConditionsRequest");
					continue;
				}
				if (_roomAuthority && (_roomAuthority->SnapshotView().tables[tableId].ready[0] ||
					_roomAuthority->SnapshotView().tables[tableId].phase != room::TablePhase::Waiting)) continue;
				(_roomAuthority ? _roomMatchData[tableId] : _matchData).rngSeed = request.rngSeed;
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETSTAGE) {
				const std::uint8_t tableId = RoomTableFor(conn);
				if ((!_roomAuthority && _matchAuthority && _matchAuthority->GetPhase() != session::MatchAuthority::Phase::Idle) ||
					(_roomAuthority && RoomMatchAuthority(tableId) && RoomMatchAuthority(tableId)->GetPhase() != session::MatchAuthority::Phase::Idle)) continue;
				// P1 submits the stage before its Ready message. P2 may already be ready.
				if ((_roomAuthority && _roomAuthority->SnapshotView().tables[tableId].ready[0]) ||
					(!_roomAuthority && _matchData.readyMessageNum[0] != -1)) continue;
				int side = -1;
				side = _roomAuthority ? RoomSideFor(conn, tableId) : -1;
				if (!_roomAuthority) for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) { side = i; break; }
				}
				if (side != 0) {
					spdlog::info("Server: sender {} tried to set stage, but is not P1", conn);
					continue;
				}
				SessionProtocol::PreBattleSetStage request;
				try {
					msg.get_to(request);
				}
				catch (const json::exception&) {
					spdlog::info("Server: could not deserialize SetConditionsRequest");
					continue;
				}

				(_roomAuthority ? _roomMatchData[tableId] : _matchData).stageID = request.stageID;
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_LOBBY_SETSETTINGS) {
				if (_roomAuthority) continue;
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side != 0) {
					spdlog::info("Server: sender {} tried to set lobby settings, but is not P1", conn);
					continue;
				}
				SessionProtocol::LobbySetSettings request;
				try {
					if (_matchAuthority) {
						const auto& rounds = msg.at("roundCount");
						const auto& time = msg.at("roundTime");
						// Validate before get_to narrows JSON numbers into game fields.
						if (!rounds.is_number_integer() || rounds < 1 || rounds > 99 ||
							!msg.at("editionSelect").is_boolean() || !msg.at("trainingMode").is_boolean() ||
							!time.at("integral").is_number_integer() || time.at("integral") < 30 || time.at("integral") > 9999 ||
							!time.at("fractional").is_number_integer() || time.at("fractional") != 0) continue;
					}
					msg.get_to(request);
				}
				catch (const json::exception&) {
					spdlog::info("Server: could not deserialize LobbySetSettings");
					continue;
				}

				// Once readiness begins, changing settings would invalidate the
				// conditions the other player has accepted. Keep legacy comparison
				// behavior while enforcing this gate on authorized Iroh rooms.
				if (_matchAuthority) {
					netplay::LobbySettings settings;
					settings.editionSelect = request.editionSelect;
					settings.roundCount = request.roundCount;
					settings.roundTime = request.roundTime.integral;
					if (!settings.Valid() || request.roundTime.fractional != 0 || request.trainingMode ||
						_matchAuthority->GetPhase() != session::MatchAuthority::Phase::Idle ||
						_matchData.readyMessageNum[0] != -1 || _matchData.readyMessageNum[1] != -1) continue;
				}
				_lobbyData.editionSelect = request.editionSelect;
				_lobbyData.roundCount = request.roundCount;
				_lobbyData.roundTime = request.roundTime;
				_lobbyData.trainingMode = request.trainingMode;
				spdlog::info(
					"Server: lobby settings set by host: edition={} rounds={} time={} training={}",
					request.editionSelect,
					request.roundCount,
					request.roundTime.integral,
					request.trainingMode
				);
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_BATTLE_LOADED) {
				if (_roomAuthority) {
					const auto tableId = RoomTableFor(conn);
					if (!RoomMatchAuthority(tableId) || RoomMatchAuthority(tableId)->GetPhase() == session::MatchAuthority::Phase::Idle ||
						!IsRoomTableParticipant(conn, tableId)) continue;
					_roomBattleLoaded[tableId].insert(conn);
					const auto& table = _roomAuthority->SnapshotView().tables[tableId];
					std::set<session::Connection> expected;
					const auto add = [&](room::MemberId id) {
						for (const auto& member : roomMembers) if (member.second == id) { expected.insert(member.first); break; }
					};
					add(table.p1); add(table.p2); for (const auto spectator : table.spectators) add(spectator);
					if (!expected.empty() && std::all_of(expected.begin(), expected.end(), [&](session::Connection peer) {
						return _roomBattleLoaded[tableId].count(peer) != 0;
					})) {
						SendRoomTable(tableId, json(SessionProtocol::BattleSynced()));
					}
				} else {
					bSendBattleSynced = true;
					for (int i = 0; i < clients.size(); i++) {
						if (clients.at(i).conn == conn) clients.at(i).data.flags |= SessionProtocol::MF_BATTLE_LOADED;
						bSendBattleSynced = bSendBattleSynced && (clients.at(i).data.flags & SessionProtocol::MF_BATTLE_LOADED);
					}
					_dataDirty = true;
				}
			}
			else if (type == SessionProtocol::MT_LOBBY_READY) {
				if (_roomAuthority) continue;
				if (_matchAuthority && _matchAuthority->GetPhase() != session::MatchAuthority::Phase::Idle) continue;
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side == -1) {
					spdlog::info("Server: sender {} tried to ready, but is not playing", conn);
					continue;
				}

				SessionProtocol::LobbyReady request;
				try {
					msg.get_to(request);
				}
				catch (const json::exception&) {
					spdlog::info("Server: could not deserialize ReportResultsRequest");
					continue;
				}
				if (!selection::Valid(selection::FromNative(_matchData.chara[side]), _lobbyData.editionSelect)) continue;
				_matchData.readyMessageNum[side] = incoming.messageId;
				bSendLobbyAllReady = bSendLobbyAllReady || _matchData.IsAllReady();
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_LOBBY_REPORTRESULTS) {
				if (_roomAuthority) continue;
				if (_matchAuthority && (_matchAuthority->GetPhase() != session::MatchAuthority::Phase::Started ||
					!msg.contains("generation") || !msg["generation"].is_number_unsigned() ||
					msg["generation"].get<std::uint64_t>() != _matchAuthority->Generation())) continue;
				SessionProtocol::LobbyReportResults request;
				try {
					msg.get_to(request);
				}
				catch (const json::exception&) {
					spdlog::info("Server: could not deserialize ReportResultsRequest");
					continue;
				}

				// Results can rotate only a player that actually exists, and
				// only either active player may report them.
				if (clients.size() < 2 || request.loserSide < 0 || request.loserSide > 1 ||
					(clients[0].conn != conn && clients[1].conn != conn)) continue;
				HandleResults(request.loserSide);
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_LOBBY_RESET) {
				if (_roomAuthority) continue;
				if (_matchAuthority && (!msg.contains("generation") || !msg["generation"].is_number_unsigned() ||
					msg["generation"].get<std::uint64_t>() != _matchAuthority->Generation())) continue;
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side == -1) {
					spdlog::info("Server: sender {} tried to reset lobby, but is not playing", conn);
					continue;
				}
				ResetLobbyForRematch();
			}
			else if (
				type == SessionProtocol::MT_BATTLE_SNAPSHOT ||
				type == SessionProtocol::MT_BATTLE_HASH
			) {
				// Forward verification payloads (legacy snapshots and v2
				// hash checkpoints) to every other client. Forwarding is
				// deliberately identical for both: the receiving client
				// decides what a mismatch means (player vs spectator).
				const auto sourceTable = _roomAuthority ? RoomTableFor(conn) : static_cast<std::uint8_t>(room::TableCount);
				if (_roomAuthority && (!RoomMatchAuthority(sourceTable) || RoomMatchAuthority(sourceTable)->GetPhase() == session::MatchAuthority::Phase::Idle ||
					!IsRoomTableParticipant(conn, sourceTable))) continue;
				for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
						if (clientIter->conn != conn && (!_roomAuthority || IsRoomTableParticipant(clientIter->conn, sourceTable))) {
							Respond(clientIter->conn, msg);
					}
				}
			}
			else if (type == SessionProtocol::MT_PUNCH_READY) {
				if (_roomAuthority) {
					const auto tableId = RoomTableFor(conn);
					if (!RoomMatchAuthority(tableId) || RoomMatchAuthority(tableId)->GetPhase() == session::MatchAuthority::Phase::Idle ||
						!IsRoomTableParticipant(conn, tableId)) continue;
					_roomPunchReady[tableId].insert(conn);
					const auto& table = _roomAuthority->SnapshotView().tables[tableId];
					std::set<session::Connection> expected;
					const auto add = [&](room::MemberId id) { for (const auto& pair : roomMembers) if (pair.second == id) { expected.insert(pair.first); break; } };
					add(table.p1); add(table.p2); for (const auto spectator : table.spectators) add(spectator);
					if (!expected.empty() && std::all_of(expected.begin(), expected.end(), [&](session::Connection peer) { return _roomPunchReady[tableId].count(peer) != 0; })) {
						SendRoomTable(tableId, json(SessionProtocol::PunchGo()));
						_roomPunchReady[tableId].clear();
					}
					continue;
				}
				int side = -1;
				for (int i = 0; i < 2 && i < (int)clients.size(); i++) {
					if (clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side == -1) {
					spdlog::info("Server: sender {} sent punch_ready but is not playing", conn);
					continue;
				}
				_punchReady[side] = true;
				if (_punchReady[0] && _punchReady[1]) {
					SessionProtocol::PunchGo go;
					json goMsg = go;
					BroadcastMessage(goMsg);
					_punchReady[0] = false;
					_punchReady[1] = false;
				}
			}
			else if (type == SessionProtocol::MT_BATTLE_GGPO_FRAME) {
				SessionProtocol::BattleGgpoFrame frame;
				try {
					msg.get_to(frame);
				}
				catch (const json::exception&) {
					spdlog::debug("Server: could not deserialize GGPO frame");
					continue;
				}

				if (!(frame.src == cid)) {
					spdlog::debug("Server: dropping fraudulent GGPO frame");
					continue;
				}
				const auto sourceTable = _roomAuthority ? RoomTableFor(conn) : static_cast<std::uint8_t>(room::TableCount);
				if (_roomAuthority && (!RoomMatchAuthority(sourceTable) || RoomMatchAuthority(sourceTable)->GetPhase() == session::MatchAuthority::Phase::Idle ||
					!IsRoomTableParticipant(conn, sourceTable))) continue;

				for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
					if (clientIter->data.connId == frame.dest && (!_roomAuthority || IsRoomTableParticipant(clientIter->conn, sourceTable))) {
						Respond(clientIter->conn, msg);
						break;
					}
				}
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
