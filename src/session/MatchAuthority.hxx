#pragma once
#include "sf4e__SessionProtocol.hxx"
#include "SessionRecovery.hxx"
#include "SessionTransport.hxx"
#include <array>
#include <functional>
#include <set>

namespace sf4e { namespace session {

constexpr std::size_t GgpoInputBytes = 8;
// Conservative full-queue bound from the pinned GGPO fork: a 32-byte input
// prefix, 64 queued frames, two players in a spectator stream, and at most
// one encoding flag plus the raw input bits per frame. The game asserts the
// input layout below at compile time. Path admission checks this entire bound.
constexpr std::size_t GgpoMaximumPacket = 32 + (64 * (2 * GgpoInputBytes * 8 + 1) + 7) / 8;

class MatchAuthority {
public:
	struct Participant { Connection connection; SessionProtocol::ConnectionID member; };
	using Identity = std::function<std::string(Connection)>;
	using Send = std::function<bool(Connection, const nlohmann::json&)>;
	enum class Phase { Idle, Preparing, Connecting, Started };
	MatchAuthority(std::array<std::uint8_t, 16> room, Identity identity) : room_(room), identity_(std::move(identity)) {}
	bool Begin(const std::vector<Participant>& participants, const Send& send);
	bool BeginAtGeneration(const std::vector<Participant>& participants, std::uint64_t generation, const Send& send);
	bool Acknowledge(Connection connection, const nlohmann::json& message, const Send& send);
	bool End(const Send& send);
	// Leadership loss invalidates only pre-start mappings. A Started session
	// owns its capabilities and is intentionally left intact.
	void CancelPreparation();
	using RebindEntry = std::pair<SessionProtocol::ConnectionID, Connection>;
	// Atomically replace every process-local handle from stable endpoint
	// identities. This is required for a simultaneous permutation (old 1->2,
	// old 2->1); sequential RebindConnection calls would falsely collide.
	bool RebindConnections(const std::vector<RebindEntry>& mapping);
	bool RebindConnection(const SessionProtocol::ConnectionID& endpoint, Connection connection);
	bool MemberDeparted(const Send& send);
	bool MemberDeparted(Connection connection, const Send& send);
	// A Started spectator remains in the frozen native roster after its control
	// member leaves or switches tables. Resolve this from the authority's
	// authenticated endpoint list rather than the mutable room projection.
	bool HasStartedSpectator(const SessionProtocol::ConnectionID& endpoint) const;
	Phase GetPhase() const { return phase_; }
	std::uint64_t Generation() const { return generation_; }
	nlohmann::json Checkpoint() const;
	bool RestoreCheckpoint(const nlohmann::json& value);
	// Portable form omits process-local Connection values. The resolver is
	// called only during rebind and must return a currently attached handle for
	// the authenticated endpoint.
	using Rebind = std::function<Connection(const SessionProtocol::ConnectionID&)>;
	nlohmann::json PortableCheckpoint() const;
	bool RestorePortableCheckpoint(const nlohmann::json& value, const Rebind& rebind);
    const std::string& CapabilityDigest() const { return capabilityDigest_; }
private:
	bool Broadcast(const nlohmann::json& message, const Send& send);
	std::array<std::uint8_t, 16> room_;
	Identity identity_;
	Phase phase_ = Phase::Idle;
	std::uint64_t generation_ = 0;
	std::vector<Participant> participants_;
	std::set<Connection> acknowledgments_;
	std::set<Connection> departed_;
	// Pair capabilities never cross the replicated checkpoint. This digest is
	// enough to detect an accidental regeneration while keeping the secret
	// material private to the two native endpoints.
	std::string capabilityDigest_;
};

} }
