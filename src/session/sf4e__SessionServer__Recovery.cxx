// SessionServer: room and recovery checkpoints, the effect journal, quorum proposals and member rebinding.

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
constexpr std::size_t kRecoveryProposalBytes = 1024 * 1024;

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
		if (json(history).dump().size() > session::MaxEffectJournalBytes) return false;
		const auto authority = value.value("authority", json::object());
		if (authority.empty()) return RestoreRecoveryState(value, std::move(history), nullptr);
		const session::AuthorityStamp stamp{authority.value("term", 0ULL), authority.value("revision", 0ULL), authority.value("writable", false)};
		return RestoreRecoveryState(value, std::move(history), &stamp);
	} catch (const std::exception&) { return false; }
}

bool SessionServer::RestoreRecoveryCheckpoint(const json& value,
	std::vector<session::EffectEnvelope> journal, const session::AuthorityStamp& authority) {
	return RestoreRecoveryState(value, std::move(journal), &authority);
}

bool SessionServer::RestoreRecoveryState(const json& value,
	std::vector<session::EffectEnvelope> incomingHistory, const session::AuthorityStamp* authority) {
	try {
		if (value.value("schema", std::string()) != "session-recovery-v2" || !value.at("room").is_object() ||
			!value.at("members").is_array() || value.at("members").size() > room::MaximumMembers + room::TableCount * room::MaxMatchParticipants) return false;
		if (incomingHistory.size() > session::MaxEffectJournalEntries) return false;
		for (const auto& effect : incomingHistory)
			if (effect.term == 0 || effect.recipient == 0 || !session::recovery_detail::IsSha256(effect.payloadDigest)) return false;
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
			return session::MatchAuthority::LocalCheckpoint(portable, [&](const SessionProtocol::ConnectionID& endpoint) {
				auto iter = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& p) { return p.second == endpoint; });
				return iter == endpoints.end() ? session::Connection(0) : synthetic.at(iter->first);
			});
		};
		json authorities = json::array();
		for (const auto& authority : value.at("match_authorities")) { auto converted = convertAuthority(authority); if (converted.is_discarded()) return false; authorities.push_back(std::move(converted)); }
		legacy["match_authorities"] = std::move(authorities);
		legacy["legacy_authority"] = convertAuthority(value.value("legacy_authority", json(nullptr)));
		if (legacy["legacy_authority"].is_discarded()) return false;
		const auto previousAuthorityTerm = _recovery.Authority().term;
		if (!RestoreCheckpoint(legacy)) return false;
		if (_roomAuthority && !_roomAuthority->RecoveryPaused()) _roomAuthority->PauseForRecovery();
		_committedEffectHistory = std::move(incomingHistory);
		_committedEffectSizes.clear();
		_incarnation = value.value("incarnation", 1ULL);
		if (authority) {
			_recovery.SetAuthority(authority->term, authority->revision, authority->writable);
			if (authority->term && authority->term != previousAuthorityTerm) _preparationCancellationRequested = true;
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
		_candidate = {};
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
		if (_candidate.effects.empty() && RecoveryCheckpoint() == _recoveryBaseline) {
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
	_candidate = {};
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
	_candidate = {}; _recoveryCandidateReady = false; _recoveryBaseline.clear();
	_recoveryBaselineBindings.clear();
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
	diag::ScopedTimer timer(diag::OP_ROOM_JOURNAL);
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
	// Encoded once: for the digest here, the journal size below, and the live
	// send in ApplyCommit.
	std::string encoded = payload.dump();
	envelope.payloadDigest = session::recovery_detail::Sha256(encoded);
	const auto type = envelope.type;
	const bool publicReplay = type == "room_snapshot" || type == "room_result" || type == "room_event" ||
		type == "data_update" || type == "game_start" || type == "game_end" || type == "game_peer_end";
	// game_connect remains a private native transition. Pair capabilities and
	// connection setup are never replayed from a replicated payload.
	envelope.privatePayload = !publicReplay || session::ContainsCapabilityField(payload);
	if (!envelope.privatePayload && retainPublicReplay) envelope.publicPayload = payload;
	auto& effects = _candidate.effects;
	if (type == "room_snapshot" || type == "data_update") {
		for (std::size_t i = 0; i < effects.size();) {
			if (effects[i].envelope.recipient == envelope.recipient && effects[i].envelope.type == type) {
				_candidate.bytes -= effects[i].envelopeBytes;
				if (effects.size() > 1) --_candidate.bytes;
				effects.erase(effects.begin() + i);
			} else ++i;
		}
	}
	for (const auto& effect : effects) {
		const auto& prior = effect.envelope;
		if (prior.recipient == envelope.recipient && prior.revision == envelope.revision && prior.type == envelope.type && prior.payloadDigest == envelope.payloadDigest) return;
	}
	auto envelopeBytes = session::EncodedEnvelopeBytes(envelope, encoded.size());
	const auto appended = [&](std::size_t bytes) { return _candidate.bytes + (effects.empty() ? 0 : 1) + bytes; };
	const auto fits = [&](std::size_t bytes) {
		return effects.size() + 1 <= session::MaxEffectJournalEntries && appended(bytes) <= session::MaxEffectJournalBytes;
	};
	// A projection's replay copy is optional; its digest still authenticates
	// the live delivery. When the candidate is full, drop this copy rather than
	// run the whole-candidate pass below, which re-encodes every entry and in a
	// large room would run again for each remaining recipient. That pass is for
	// making room for a record that must be kept whole.
	if (!fits(envelopeBytes) && session::SupersedableProjection(envelope) && !envelope.publicPayload.is_null()) {
		envelope.publicPayload = nullptr;
		envelopeBytes = session::EncodedEnvelopeBytes(envelope, encoded.size());
	}
	if (fits(envelopeBytes)) {
		_candidate.bytes = appended(envelopeBytes);
		effects.push_back({std::move(envelope), client, std::move(encoded), envelopeBytes});
		return;
	}
	auto prospective = _candidate.Envelopes();
	prospective.push_back(envelope);
	const auto prospectiveBytes = session::ShedOptionalEffectPayloads(prospective, session::MaxEffectJournalBytes);
	if (prospective.size() > session::MaxEffectJournalEntries || prospectiveBytes > session::MaxEffectJournalBytes) {
		_candidate.overflow = true;
		return;
	}
	// Existing local payloads remain intact; only an envelope's optional replay
	// copy may have been shed by the whole-candidate pass.
	effects.push_back({session::EffectEnvelope{}, client, std::move(encoded), 0});
	for (std::size_t i = 0; i < effects.size(); ++i) {
		effects[i].envelope = std::move(prospective[i]);
		effects[i].envelopeBytes = nlohmann::json(effects[i].envelope).dump().size();
	}
	_candidate.bytes = prospectiveBytes;
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
	if (_candidate.overflow) return false;
	json local;
	try { local = RecoveryCheckpoint(); } catch (...) { return false; }
	// Root may attach its own metadata, but state must be byte-for-byte the
	// candidate produced by this owner before it can be committed.
	if (!checkpoint.is_null() && checkpoint != local) return false;
	session::SessionProposal proposal;
	proposal.request = request; proposal.term = term; proposal.baseRevision = baseRevision;
	proposal.checkpoint = std::move(local); proposal.effects = _candidate.Envelopes();
	// The effects are encoded once, for their digest and inside the proposal.
	const auto encodedEffects = json(proposal.effects).dump();
	proposal.effectsDigest = session::recovery_detail::Sha256(encodedEffects);
	proposal.encoded = session::EncodeSessionProposal(proposal, encodedEffects);
	if (proposal.encoded.size() > kRecoveryProposalBytes) return false;
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
	{
		diag::ScopedTimer sendTimer(diag::OP_ROOM_COMMIT_SEND);
		for (const auto& effect : _candidate.effects) {
			session::Connection local = effect.local;
			if (!ValidateEffectRecipient(effect.envelope, effect.local, local)) continue; // departed member; helper owns the removal decision
			// The client still consumes the original protocol object.  The commit
			// envelope is additive and gives the recipient enough immutable material
			// to reject a stale/mutated replay before dispatching the payload. The
			// token has the same shape as a checkpoint journal entry, so the
			// recipient parses one form for both.
			if (!_transport || !_transport->Send(local, session::CommittedEffectWire(effect.envelope, effect.encoded))) _transportFailed = true;
		}
	}
	_recoveryFlushing = false;
	for (auto& client : clients) {
		const auto sent = _candidate.chatSent.find(client.conn);
		if (sent != _candidate.chatSent.end()) { client.chatSent = true; client.chatVersion = sent->second; }
	}
	diag::ScopedTimer compactTimer(diag::OP_ROOM_COMPACT);
	// Sizes are unknown once after a restore; measure the inherited journal once.
	if (_committedEffectSizes.size() != _committedEffectHistory.size()) {
		_committedEffectSizes.clear();
		for (const auto& effect : _committedEffectHistory) _committedEffectSizes.push_back(json(effect).dump().size());
	}
	for (auto& effect : _candidate.effects) {
		_committedEffectHistory.push_back(std::move(effect.envelope));
		_committedEffectSizes.push_back(effect.envelopeBytes);
	}
	// Keep lifecycle/final-result effects in order while compacting only
	// supersedable projections. The same helper is used by the root recovery
	// bridge when it merges a checkpoint journal with a committed proposal.
	session::CompactEffectJournal(_committedEffectHistory, _committedEffectSizes);
	_candidate = {}; _recoveryCandidateReady = false; _recoveryBaseline.clear(); _recoveryBaselineBindings.clear();
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

std::size_t SessionServer::ReplayPendingTerminalEvents(session::Connection connection, room::MemberId member) {
	if (!_roomAuthority || !connection || !member) return 0;
	const auto pending = _roomAuthority->PendingTerminalEvents(member);
	for (const auto& replay : pending) {
		SessionProtocol::RoomEventMessage eventMessage;
		eventMessage.event = room::Event{room::Event::Kind::MatchEnded, replay.table, replay.generation, 0, replay.result, true};
		Respond(connection, json(eventMessage));
		// The native side may also still be waiting for its game_end.
		if (!MatchSender()(connection, json{{"type", "game_end"}, {"generation", replay.generation}})) _transportFailed = true;
		spdlog::info("Server: replayed terminal event member={} table={} generation={} result={}",
			member, replay.table, replay.generation, static_cast<int>(replay.result));
	}
	return pending.size();
}
