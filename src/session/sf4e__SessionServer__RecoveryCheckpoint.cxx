// SessionServer: the legacy and portable recovery checkpoints and their restore.

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
session::Connection SyntheticConnection(room::MemberId member) {
	return (std::numeric_limits<session::Connection>::max)() - member;
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
			return RefuseRestore("header");
		// The temporary owns no transport. Rejected imports cannot partially
		// replace live state, disconnect peers, or publish native match effects.
		SessionServer candidate(_identity, _sidecarHash, true, 3, {0, 99}, nullptr);
		candidate._roomAuthority.reset(new room::RoomAuthority("Recovering room"));
		if (!candidate._roomAuthority->RestoreCheckpoint(value.at("room"))) return RefuseRestore("room");
		const auto& snapshot = candidate._roomAuthority->SnapshotView();
		const auto readMap = [&](const char* key, auto& destination, std::size_t maximum) {
			const auto& rows = value.at(key);
			if (!rows.is_array() || rows.size() > maximum) return RefuseRestore(key);
			for (const auto& row : rows) {
				if (!row.is_array() || row.size() != 2 || !row[0].is_number_unsigned()) return RefuseRestore(key);
				using Map = typename std::decay<decltype(destination)>::type;
				const auto keyValue = row[0].get<typename Map::key_type>();
				if (static_cast<std::uint64_t>(keyValue) != row[0].get<std::uint64_t>() ||
					!destination.emplace(keyValue, row[1].get<typename Map::mapped_type>()).second) return RefuseRestore(key);
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
				!cids.emplace(cid.host, cid.user).second) return RefuseRestore("cids");
		}
		std::set<room::MemberId> memberIds;
		for (const auto& mapping : candidate.roomMembers) {
			const auto member = std::find_if(snapshot.members.begin(), snapshot.members.end(),
				[&](const room::Member& m) { return m.id == mapping.second; });
			const auto cid = candidate.cidMap.find(mapping.first);
			if (member == snapshot.members.end() || cid == candidate.cidMap.end() ||
				member->connection.host != cid->second.host || member->connection.user != cid->second.user ||
				!memberIds.insert(mapping.second).second) return RefuseRestore("room_members");
		}
		if (memberIds.size() != snapshot.members.size()) return RefuseRestore("room_members");
		for (const auto& mapping : candidate.roomSelectedTables)
			if (!candidate.roomMembers.count(mapping.first) || mapping.second >= room::TableCount) return RefuseRestore("selected_tables");
		for (const auto& mapping : candidate._roomMatchData)
			if (mapping.first >= room::TableCount) return RefuseRestore("table_matches");
		std::set<std::string> identities;
		for (const auto& mapping : candidate.roomPeerIdentities) {
			if (!memberIds.count(mapping.first) || mapping.second.empty() || mapping.second.size() > 256 ||
				!identities.insert(mapping.second).second) return RefuseRestore("peer_identities");
		}
		if (candidate.roomPeerIdentities.size() != memberIds.size()) return RefuseRestore("peer_identities");
		const auto& bans = value.at("bans");
		if (!bans.is_array()) return RefuseRestore("bans");
		for (const auto& entry : bans) {
			const auto identity = entry.get<std::string>();
			if (identity.empty() || identity.size() > 256 || identities.count(identity) ||
				!candidate.roomBannedIdentities.insert(identity).second) return RefuseRestore("bans");
		}
		const auto& members = value.at("members");
		if (!members.is_array() || members.size() != memberIds.size()) return RefuseRestore("members");
		std::set<session::Connection> connections;
		for (const auto& row : members) {
			if (!row.at("connection").is_number_unsigned() || !row.at("flags").is_number_unsigned()) return RefuseRestore("members");
			SessionMember member{};
			member.conn = row.at("connection").get<session::Connection>();
			member.data = row.at("data").get<SessionProtocol::MemberData>();
			member.data.flags = row.at("flags").get<std::uint64_t>();
			if (!candidate.roomMembers.count(member.conn) || !connections.insert(member.conn).second ||
				!(candidate.cidMap.at(member.conn) == member.data.connId) ||
				member.data.name.size() > 256 || member.data.ip.size() > 256 ||
				(member.data.flags & ~std::uint64_t(SessionProtocol::MF_BATTLE_LOADED))) return RefuseRestore("members");
			candidate.clients.push_back(std::move(member));
		}
		// One normalized identity per member feeds its client row, the incarnation
		// map, its stable record and the room snapshot. An old row without an
		// incarnation takes the snapshot member's, which old snapshots read as 1.
		for (auto& row : candidate.clients) {
			const auto member = candidate.roomMembers.at(row.conn);
			const auto snap = std::find_if(snapshot.members.begin(), snapshot.members.end(),
				[&](const room::Member& value) { return value.id == member; });
			const auto incarnation = row.data.incarnation ? row.data.incarnation : snap->incarnation;
			row.data.incarnation = incarnation;
			row.data.roomMember = member;
			row.data.authenticatedEndpoint = candidate.roomPeerIdentities.at(member);
			candidate._roomAuthority->SetMemberIncarnation(member, incarnation);
			candidate.roomIncarnations[member] = incarnation;
			candidate.roomMemberRecords[member] = row.data;
		}
		const auto readSets = [&](const char* key, auto& destination) {
			const auto& rows = value.at(key);
			if (!rows.is_array() || rows.size() != room::TableCount) return RefuseRestore(key);
			for (std::size_t i = 0; i < room::TableCount; ++i) {
				if (!rows[i].is_array() || rows[i].size() > room::MaxMembers) return RefuseRestore(key);
				for (const auto& entry : rows[i]) {
					if (!entry.is_number_unsigned()) return RefuseRestore(key);
					const auto connection = entry.get<session::Connection>();
					if (!connections.count(connection) || !destination[i].insert(connection).second) return RefuseRestore(key);
				}
			}
			return true;
		};
		if (!readSets("battle_loaded", candidate._roomBattleLoaded) ||
			!readSets("punch_ready", candidate._roomPunchReady)) return false;
		candidate._lobbyData = value.at("lobby").get<SessionProtocol::LobbyData>();
		if (candidate._lobbyData.id.host != _identity || candidate._lobbyData.members.size() > room::MaxMembers) return RefuseRestore("lobby");
		for (std::size_t i = 0; i < candidate._lobbyData.members.size(); ++i)
			candidate._lobbyData.members[i].flags = value.at("lobby").at("members")[i].at("flags").get<std::uint64_t>();
		candidate._matchData = value.at("match").get<SessionProtocol::MatchData>();
		const auto punch = value.at("legacy_punch_ready").get<std::array<bool, 2>>();
		candidate._dataDirty = value.at("dirty").get<bool>();
		const auto& authorities = value.at("match_authorities");
		if (!authorities.is_array() || authorities.size() != room::TableCount) return RefuseRestore("match_authorities");
		const auto restoreAuthority = [&](const json& data, std::unique_ptr<session::MatchAuthority>& authority) {
			if (data.is_null()) return !_matchAuthorizationConfigured;
			if (!_matchAuthorizationConfigured) return false;
			authority.reset(new session::MatchAuthority(_matchAuthorizationRoom, _matchAuthorizationIdentity));
			return authority->RestoreCheckpoint(data);
		};
		if (!restoreAuthority(value.at("legacy_authority"), candidate._matchAuthority)) return RefuseRestore("legacy_authority");
		for (std::size_t i = 0; i < room::TableCount; ++i)
			if (!restoreAuthority(authorities[i], candidate._roomMatchAuthorities[i]))
				return RefuseRestore("match_authority " + std::to_string(i));
		_roomAuthority = std::move(candidate._roomAuthority);
		clients = std::move(candidate.clients); cidMap = std::move(candidate.cidMap);
		roomMembers = std::move(candidate.roomMembers); roomSelectedTables = std::move(candidate.roomSelectedTables);
		roomPeerIdentities = std::move(candidate.roomPeerIdentities); roomBannedIdentities = std::move(candidate.roomBannedIdentities);
		roomIncarnations = std::move(candidate.roomIncarnations); roomMemberRecords = std::move(candidate.roomMemberRecords);
		roomFrozenMembers.clear();
		_lobbyData = std::move(candidate._lobbyData); _matchData = candidate._matchData;
		_roomMatchData = std::move(candidate._roomMatchData);
		_roomBattleLoaded = std::move(candidate._roomBattleLoaded); _roomPunchReady = std::move(candidate._roomPunchReady);
		_matchAuthority = std::move(candidate._matchAuthority); _roomMatchAuthorities = std::move(candidate._roomMatchAuthorities);
		_punchReady[0] = punch[0]; _punchReady[1] = punch[1]; _dataDirty = candidate._dataDirty;
		_departingConnections.clear(); _afterDataMessages.clear();
		return true;
	} catch (const std::exception& error) { return RefuseRestore(std::string("checkpoint: ") + error.what()); }
}

json SessionServer::RecoveryCheckpoint() const {
	diag::ScopedTimer timer(diag::OP_ROOM_CHECKPOINT_BUILD);
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
		const auto record = roomMemberRecords.find(member.id);
		if (record != roomMemberRecords.end()) data.latency = record->second.latency;
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
	// Uncommitted effects travel once, in SessionProposal.effects.
	value["effect_journal"] = _committedEffectHistory;
	return value;
}

bool SessionServer::RestoreRecoveryCheckpoint(const json& value) {
	try {
		auto history = value.value("effect_journal", std::vector<session::EffectEnvelope>{});
		// The same size as json(history).dump(), without a JSON copy of the journal.
		if (session::EffectJournalBytes(history) > session::MaxEffectJournalBytes) return RefuseRestore("effect_journal");
		const auto authority = value.value("authority", json::object());
		if (authority.empty()) return RestoreRecoveryState(value, std::move(history), nullptr);
		const session::AuthorityStamp stamp{authority.value("term", 0ULL), authority.value("revision", 0ULL), authority.value("writable", false)};
		return RestoreRecoveryState(value, std::move(history), &stamp);
	} catch (const std::exception& error) { return RefuseRestore(std::string("recovery: ") + error.what()); }
}

bool SessionServer::RestoreRecoveryCheckpoint(const json& value,
	std::vector<session::EffectEnvelope> journal, const session::AuthorityStamp& authority) {
	return RestoreRecoveryState(value, std::move(journal), &authority);
}

bool SessionServer::RestoreRecoveryState(const json& value,
	std::vector<session::EffectEnvelope> incomingHistory, const session::AuthorityStamp* authority) {
	try {
		if (value.value("schema", std::string()) != "session-recovery-v2" || !value.at("room").is_object() ||
			!value.at("members").is_array() || value.at("members").size() > room::MaximumMembers + room::TableCount * room::MaxMatchParticipants)
			return RefuseRestore("schema");
		if (incomingHistory.size() > session::MaxEffectJournalEntries) return RefuseRestore("effect_journal");
		for (const auto& effect : incomingHistory)
			if (effect.term == 0 || effect.recipient == 0 || !session::recovery_detail::IsSha256(effect.payloadDigest))
				return RefuseRestore("effect_journal");
		// Everything but the recovery envelope. The journal is the bulk of a
		// checkpoint and RestoreCheckpoint does not read it, so it is not copied.
		json legacy = json::object();
		for (auto field = value.begin(); field != value.end(); ++field)
			if (field.key() != "schema" && field.key() != "authority" && field.key() != "effect_journal" && field.key() != "incarnation")
				legacy[field.key()] = field.value();
		json legacyMembers = json::array(), cids = json::array(), roomMemberRows = json::array(), selected = json::array();
		std::map<room::MemberId, session::Connection> synthetic;
		std::map<room::MemberId, SessionProtocol::ConnectionID> endpoints;
		std::array<std::set<room::MemberId>, room::TableCount> pendingBattleLoaded{};
		std::array<std::set<room::MemberId>, room::TableCount> pendingPunchReady{};
		for (const auto& row : value.at("members")) {
			const auto member = row.at("member").get<room::MemberId>();
			const auto endpoint = row.at("endpoint").get<room::ConnectionRef>();
			if (!member || !synthetic.emplace(member, SyntheticConnection(member)).second) return RefuseRestore("members");
			SessionProtocol::MemberData data = row.at("data").get<SessionProtocol::MemberData>();
			data.connId = {endpoint.host, endpoint.user}; data.roomMember = member;
			data.authenticatedEndpoint = row.value("authenticated_endpoint", data.authenticatedEndpoint);
			// An active row with neither incarnation field keeps 0, so the canonical
			// restore takes the snapshot member's. A frozen row has no snapshot member.
			const bool frozen = row.value("frozen", false);
			data.incarnation = frozen ? row.value("incarnation", data.incarnation ? data.incarnation : 1ULL)
				: row.value("incarnation", data.incarnation);
			if (!frozen) {
				legacyMembers.push_back({{"connection", synthetic.at(member)}, {"data", data}, {"flags", data.flags}});
				cids.push_back({synthetic.at(member), data.connId});
				roomMemberRows.push_back({synthetic.at(member), member});
			}
			endpoints.emplace(member, data.connId);
		}
		legacy["members"] = std::move(legacyMembers); legacy["cids"] = std::move(cids); legacy["room_members"] = std::move(roomMemberRows);
		for (const auto& row : value.value("selected_members", json::array())) {
			const auto member = row.at("member").get<room::MemberId>();
			if (!synthetic.count(member) || row.at("table").get<std::uint64_t>() >= room::TableCount) return RefuseRestore("selected_members");
			selected.push_back({synthetic.at(member), row.at("table")});
		}
		legacy["selected_tables"] = std::move(selected);
		const auto readPendingSets = [&](const char* key, auto& target) {
			const auto rows = value.value(key, json::array());
			if (!rows.is_array() || rows.size() != room::TableCount) return RefuseRestore(key);
			for (std::size_t i = 0; i < room::TableCount; ++i) {
				if (!rows[i].is_array()) return RefuseRestore(key);
				for (const auto& id : rows[i]) {
					const auto member = id.get<room::MemberId>();
					if (!synthetic.count(member) || !target[i].insert(member).second) return RefuseRestore(key);
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
		if (decodeSets("battle_loaded_members", "battle_loaded").is_null() || decodeSets("punch_ready_members", "punch_ready").is_null())
			return RefuseRestore("pending_sets");
		const auto convertAuthority = [&](const json& portable) {
			if (portable.is_null()) return json(nullptr);
			return session::MatchAuthority::LocalCheckpoint(portable, [&](const SessionProtocol::ConnectionID& endpoint) {
				auto iter = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& p) { return p.second == endpoint; });
				return iter == endpoints.end() ? session::Connection(0) : synthetic.at(iter->first);
			});
		};
		json authorities = json::array();
		for (const auto& authority : value.at("match_authorities")) { auto converted = convertAuthority(authority); if (converted.is_discarded()) return RefuseRestore("match_authorities"); authorities.push_back(std::move(converted)); }
		legacy["match_authorities"] = std::move(authorities);
		legacy["legacy_authority"] = convertAuthority(value.value("legacy_authority", json(nullptr)));
		if (legacy["legacy_authority"].is_discarded()) return RefuseRestore("legacy_authority");
		const auto previousAuthorityTerm = _recovery.Authority().term;
		// A commit made before an outage must not take back the time a
		// permit's window has already run here. While a candidate is open its
		// own reservations are not committed: a successor can reserve the same
		// generation, so only the baseline's timers carry over.
		const auto permitAges = !_roomAuthority ? room::RoomAuthority::PermitTimers{}
			: _recoveryCandidateReady ? _recoveryBaselinePermits : _roomAuthority->PermitAges();
		if (!RestoreCheckpoint(legacy)) return false;
		if (_roomAuthority && !_roomAuthority->RecoveryPaused()) _roomAuthority->PauseForRecovery();
		if (_roomAuthority) _roomAuthority->KeepPermitAges(permitAges);
		_committedEffectHistory = std::move(incomingHistory);
		_committedEffectSizes.clear();
		_incarnation = value.value("incarnation", 1ULL);
		if (authority) {
			_recovery.SetAuthority(authority->term, authority->revision, authority->writable);
			if (authority->term && authority->term != previousAuthorityTerm) _preparationCancellationRequested = true;
		}
		// RestoreCheckpoint has already normalized the active incarnations and
		// records. Only frozen state is rebuilt here, and an active entry wins.
		roomFrozenMembers.clear();
		for (const auto& row : value.at("members")) {
			if (!row.value("frozen", false)) continue;
			const auto member = row.at("member").get<room::MemberId>();
			FrozenMember frozen;
			frozen.endpoint = row.at("endpoint").get<room::ConnectionRef>();
			frozen.data = row.at("data").get<SessionProtocol::MemberData>();
			const auto incarnation = row.value("incarnation", frozen.data.incarnation ? frozen.data.incarnation : 1ULL);
			roomIncarnations.emplace(member, incarnation);
			frozen.data.connId = {frozen.endpoint.host, frozen.endpoint.user};
			frozen.data.roomMember = member;
			frozen.data.incarnation = incarnation;
			frozen.incarnation = incarnation;
			roomFrozenMembers[member] = std::move(frozen);
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
	} catch (const std::exception& error) { return RefuseRestore(std::string("recovery: ") + error.what()); }
}
