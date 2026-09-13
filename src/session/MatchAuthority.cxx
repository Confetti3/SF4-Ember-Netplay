#include "MatchAuthority.hxx"
#include <bcrypt.h>
#include <algorithm>
#include <limits>

namespace sf4e { namespace session {
using nlohmann::json;

json MatchAuthority::Checkpoint() const {
    json participants = json::array();
    for (const auto& participant : participants_)
        participants.push_back({{"connection", participant.connection}, {"member", participant.member}});
    // Existing pair capabilities belong to the endpoint helpers, not to the
    // replicated authority. Recovery must never generate replacements for them.
    return {{"version", 1}, {"room", room_}, {"phase", static_cast<int>(phase_)},
        {"generation", generation_}, {"participants", participants},
        {"acknowledgments", acknowledgments_}, {"departed", departed_},
        {"cap_digest", capabilityDigest_}};
}

json MatchAuthority::PortableCheckpoint() const {
    json participants = json::array();
    for (const auto& participant : participants_)
        participants.push_back({{"endpoint", participant.member}});
    json acknowledged = json::array(), departed = json::array();
    for (const auto connection : acknowledgments_) {
        for (const auto& participant : participants_)
            if (participant.connection == connection) { acknowledged.push_back(participant.member); break; }
    }
    for (const auto connection : departed_) {
        for (const auto& participant : participants_)
            if (participant.connection == connection) { departed.push_back(participant.member); break; }
    }
    return {{"version", 1}, {"room", room_}, {"phase", static_cast<int>(phase_)},
        {"generation", generation_}, {"participants", participants},
        {"acknowledgments", acknowledged}, {"departed", departed}, {"cap_digest", capabilityDigest_}};
}

bool MatchAuthority::RestorePortableCheckpoint(const json& value, const Rebind& rebind) {
    try {
        if (!rebind || value.at("version") != 1 || value.at("room").get<decltype(room_)>() != room_ ||
            !value.at("phase").is_number_integer() || !value.at("generation").is_number_unsigned()) return false;
        const auto phase = value.at("phase").get<long long>();
        const auto generation = value.at("generation").get<std::uint64_t>();
        if (phase < 0 || phase > static_cast<int>(Phase::Started) ||
            generation > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return false;
        const auto& rows = value.at("participants");
        if (!rows.is_array() || rows.size() > room::MaxMatchParticipants ||
            (phase != 0 && (rows.size() < 2 || !generation))) return false;
        std::vector<Participant> participants; std::set<Connection> seen;
        std::set<std::pair<std::string, std::string>> identities;
        for (const auto& row : rows) {
            const auto member = row.at("endpoint").get<SessionProtocol::ConnectionID>();
            const auto connection = rebind(member);
            if (!connection || member.host.empty() || member.host.size() > 256 || member.user.empty() || member.user.size() > 256 ||
                !seen.insert(connection).second || !identities.emplace(member.host, member.user).second) return false;
            participants.push_back({connection, member});
        }
        std::set<Connection> acknowledgments, departed;
        const auto readSet = [&](const char* key, std::set<Connection>& target) {
            const auto& rows = value.at(key);
            if (!rows.is_array() || rows.size() > participants.size()) return false;
            for (const auto& row : rows) {
                const auto member = row.get<SessionProtocol::ConnectionID>();
                const auto iter = std::find_if(participants.begin(), participants.end(), [&](const Participant& p) { return p.member == member; });
                if (iter == participants.end() || !target.insert(iter->connection).second) return false;
            }
            return true;
        };
        if (!readSet("acknowledgments", acknowledgments) || !readSet("departed", departed)) return false;
        const auto digest = value.value("cap_digest", std::string());
        if (!digest.empty() && !recovery_detail::IsSha256(digest)) return false;
        phase_ = static_cast<Phase>(phase); generation_ = generation; participants_ = std::move(participants);
        acknowledgments_ = std::move(acknowledgments); departed_ = std::move(departed); capabilityDigest_ = digest;
        return true;
    } catch (const std::exception&) { return false; }
}

bool MatchAuthority::RestoreCheckpoint(const json& value) {
    try {
        if (value.at("version") != 1 || value.at("room").get<decltype(room_)>() != room_ ||
            !value.at("phase").is_number_integer() || !value.at("generation").is_number_unsigned()) return false;
        const auto phase = value.at("phase").get<long long>();
        const auto generation = value.at("generation").get<std::uint64_t>();
        if (phase < 0 || phase > static_cast<int>(Phase::Started) ||
            generation > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return false;
        const auto& rows = value.at("participants");
        if (!rows.is_array() || rows.size() > room::MaxMatchParticipants ||
            (phase != 0 && (rows.size() < 2 || !generation))) return false;
        std::vector<Participant> participants; std::set<Connection> seen;
        std::set<std::pair<std::string, std::string>> identities;
        for (const auto& row : rows) {
            if (!row.at("connection").is_number_unsigned()) return false;
            const auto connection = row.at("connection").get<Connection>();
            const auto member = row.at("member").get<SessionProtocol::ConnectionID>();
            if (!connection || member.host.empty() || member.host.size() > 256 || member.user.empty() || member.user.size() > 256 ||
                !seen.insert(connection).second || !identities.emplace(member.host, member.user).second) return false;
            participants.push_back({connection, member});
        }
        std::set<Connection> acknowledgments, departed;
        const auto readSet = [&](const char* key, std::set<Connection>& target) {
            const auto& rows = value.at(key);
            if (!rows.is_array() || rows.size() > participants.size()) return false;
            for (const auto& row : rows) {
                if (!row.is_number_unsigned()) return false;
                const auto connection = row.get<Connection>();
                if (!seen.count(connection) || !target.insert(connection).second) return false;
            }
            return true;
        };
        if (!readSet("acknowledgments", acknowledgments) || !readSet("departed", departed)) return false;
        const auto digest = value.value("cap_digest", std::string());
        if (!digest.empty() && !recovery_detail::IsSha256(digest)) return false;
        phase_ = static_cast<Phase>(phase); generation_ = generation;
        participants_ = std::move(participants); acknowledgments_ = std::move(acknowledgments); departed_ = std::move(departed);
        capabilityDigest_ = digest;
        return true;
    } catch (const std::exception&) { return false; }
}

bool MatchAuthority::Broadcast(const json& message, const Send& send) {
	bool sent = true;
	for (const auto& participant : participants_) sent = send(participant.connection, message) && sent;
	return sent;
}

bool MatchAuthority::Begin(const std::vector<Participant>& participants, const Send& send) {
	if (generation_ == (std::numeric_limits<std::uint64_t>::max)()) return false;
	return BeginAtGeneration(participants, generation_ + 1, send);
}

bool MatchAuthority::BeginAtGeneration(const std::vector<Participant>& participants,
	std::uint64_t generation, const Send& send) {
	if (phase_ != Phase::Idle || participants.size() < 2 ||
		participants.size() > room::MaxMatchParticipants || generation == 0 ||
		generation <= generation_ ||
		generation > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
		std::all_of(room_.begin(), room_.end(), [](std::uint8_t b) { return b == 0; })) return false;
	std::vector<std::string> endpoints;
	std::set<Connection> connections;
	for (const auto& participant : participants) {
		const auto endpoint = identity_(participant.connection);
		if (endpoint.size() != 64 || !std::all_of(endpoint.begin(), endpoint.end(), [](char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		}) || !connections.insert(participant.connection).second ||
			std::find(endpoints.begin(), endpoints.end(), endpoint) != endpoints.end()) return false;
		endpoints.push_back(endpoint);
	}
	generation_ = generation;
	participants_ = participants;
	acknowledgments_.clear();
	departed_.clear();
	std::vector<json> grants;
	std::vector<SessionProtocol::ConnectionID> roster;
	for (const auto& participant : participants) roster.push_back(participant.member);
	for (std::size_t slot = 0; slot < participants.size(); ++slot) {
		grants.push_back(json{{"type", "game_prepare"}, {"version", 1}, {"room", room_},
			{"generation", generation_}, {"slot", slot}, {"roster", roster}, {"max_packet", GgpoMaximumPacket},
			{"local_identity", endpoints[slot]}, {"links", json::array()}});
	}
	// P1 owns the spectator stream, matching the existing StartSpectating path.
	// Each edge has a unique capability shared only with its two endpoints.
	for (std::size_t peer = 1; peer < participants.size(); ++peer) {
		std::array<std::uint8_t, 32> capability;
		if (BCryptGenRandom(nullptr, capability.data(), static_cast<ULONG>(capability.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
			phase_ = Phase::Idle; return false;
		}
		grants[0]["links"].push_back(json{{"slot", peer}, {"peer", endpoints[peer]}, {"capability", capability}, {"dial", false}});
		grants[peer]["links"].push_back(json{{"slot", 0}, {"peer", endpoints[0]}, {"capability", capability}, {"dial", true}});
		SecureZeroMemory(capability.data(), capability.size());
	}
	// The grant JSON is still local memory and is discarded after the send
	// boundary. Only its digest is retained in replicated authority state.
	capabilityDigest_ = PayloadDigest(json(grants));
	phase_ = Phase::Preparing;
	for (std::size_t slot = 0; slot < participants.size(); ++slot) {
		if (!send(participants[slot].connection, grants[slot])) {
			phase_ = Phase::Idle;
			participants_.clear();
			acknowledgments_.clear();
			departed_.clear();
			return false;
		}
	}
	return true;
}

bool MatchAuthority::Acknowledge(Connection connection, const json& message, const Send& send) {
	if (phase_ == Phase::Idle || phase_ == Phase::Started) return true;
	const auto type = message.value("type", std::string());
	if (message.value("generation", std::uint64_t(0)) != generation_ ||
		std::none_of(participants_.begin(), participants_.end(), [&](const Participant& p) { return p.connection == connection; })) return true;
	if ((phase_ == Phase::Preparing && type != "game_prepared") ||
		(phase_ == Phase::Connecting && type != "game_ready")) return true;
	acknowledgments_.insert(connection);
	if (acknowledgments_.size() != participants_.size()) return true;
	acknowledgments_.clear();
	if (phase_ == Phase::Preparing) {
		phase_ = Phase::Connecting;
		return Broadcast(json{{"type", "game_connect"}, {"generation", generation_}}, send);
	}
	phase_ = Phase::Started;
	return Broadcast(json{{"type", "game_start"}, {"generation", generation_}}, send);
}

bool MatchAuthority::End(const Send& send) {
	if (phase_ == Phase::Idle) return true;
	const bool sent = Broadcast(json{{"type", "game_end"}, {"generation", generation_}}, send);
	phase_ = Phase::Idle;
	capabilityDigest_.clear();
	acknowledgments_.clear();
	participants_.clear();
	departed_.clear();
	capabilityDigest_.clear();
	return sent;
}

void MatchAuthority::CancelPreparation() {
	if (phase_ == Phase::Started) return;
	phase_ = Phase::Idle;
	acknowledgments_.clear();
	participants_.clear();
	departed_.clear();
	capabilityDigest_.clear();
}

bool MatchAuthority::RebindConnections(const std::vector<RebindEntry>& mapping) {
	std::vector<Participant> rebound;
	std::set<Connection> allHandles;
	std::set<std::pair<std::string, std::string>> allEndpoints;
	for (const auto& entry : mapping) {
		if (!entry.second || entry.first.host.empty() || entry.first.user.empty() ||
			!allHandles.insert(entry.second).second || !allEndpoints.emplace(entry.first.host, entry.first.user).second) return false;
	}
	std::set<Connection> handles;
	for (const auto& participant : participants_) {
		auto entry = std::find_if(mapping.begin(), mapping.end(), [&](const RebindEntry& value) {
			return value.first == participant.member;
		});
		if (entry != mapping.end()) {
			if (!handles.insert(entry->second).second) return false;
			rebound.push_back({entry->second, participant.member});
			continue;
		}
		// A Started spectator may have left the control roster while its native
		// slot remains in the frozen authority. Preserve that participant by
		// assigning a private non-transport handle; it will never be used for a
		// live send, but keeps the departed set and frozen slot addressable.
		if (!std::any_of(departed_.begin(), departed_.end(), [&](Connection value) {
			return value == participant.connection;
		})) return false;
		Connection preserved = participant.connection;
		while (allHandles.count(preserved) || handles.count(preserved)) {
			if (preserved == 1) return false;
			--preserved;
		}
		if (!handles.insert(preserved).second) return false;
		rebound.push_back({preserved, participant.member});
	}
	const auto remapSet = [&](const std::set<Connection>& source, std::set<Connection>& target) {
		for (const auto oldConnection : source) {
			auto prior = std::find_if(participants_.begin(), participants_.end(), [&](const Participant& participant) {
				return participant.connection == oldConnection;
			});
			if (prior == participants_.end()) return false;
			auto next = std::find_if(rebound.begin(), rebound.end(), [&](const Participant& participant) {
				return participant.member == prior->member;
			});
			if (next == rebound.end() || !target.insert(next->connection).second) return false;
		}
		return true;
	};
	std::set<Connection> reboundAcknowledgments, reboundDeparted;
	if (!remapSet(acknowledgments_, reboundAcknowledgments) || !remapSet(departed_, reboundDeparted)) return false;
	participants_ = std::move(rebound);
	acknowledgments_ = std::move(reboundAcknowledgments);
	departed_ = std::move(reboundDeparted);
	return true;
}

bool MatchAuthority::RebindConnection(const SessionProtocol::ConnectionID& endpoint, Connection connection) {
	if (!connection) return false;
	std::vector<RebindEntry> mapping;
	mapping.reserve(participants_.size());
	for (const auto& participant : participants_)
		mapping.push_back({participant.member, participant.connection});
	const auto found = std::find_if(mapping.begin(), mapping.end(), [&](const RebindEntry& value) { return value.first == endpoint; });
	if (found == mapping.end()) return false;
	found->second = connection;
	return RebindConnections(mapping);
}

bool MatchAuthority::MemberDeparted(const Send& send) {
	// During a running fight the independent game connections own their lifetime.
	// Loss of a room member disables coordination but does not tear them down.
	return phase_ == Phase::Started || End(send);
}

bool MatchAuthority::MemberDeparted(Connection connection, const Send& send) {
	if (phase_ == Phase::Idle || std::none_of(participants_.begin(), participants_.end(),
		[&](const Participant& participant) { return participant.connection == connection; })) {
		return true;
	}
	if (!departed_.insert(connection).second) return true;
	const auto departed = std::find_if(participants_.begin(), participants_.end(),
		[&](const Participant& participant) { return participant.connection == connection; });
	const std::size_t slot = static_cast<std::size_t>(std::distance(participants_.begin(), departed));
	// Spectators are optional links on the P1 stream once the native fight has
	// started. Their departure must not tear down the two fighter sessions.
	// Preserve the original slot in the frozen roster and tell P1 to remove
	// only that link so future capabilities cannot be re-used for the peer.
	if (phase_ == Phase::Started && slot >= 2 && !participants_.empty()) {
		return send(participants_.front().connection,
			json{{"type", "game_peer_end"}, {"generation", generation_}, {"slot", slot}});
	}
	return End(send);
}

bool MatchAuthority::HasStartedSpectator(const SessionProtocol::ConnectionID& endpoint) const {
	if (phase_ != Phase::Started) return false;
	for (std::size_t slot = 2; slot < participants_.size(); ++slot)
		if (participants_[slot].member == endpoint) return true;
	return false;
}

} }
