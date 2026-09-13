#pragma once

#include "SessionTransport.hxx"
#include "../platform/HelperClient.hxx"
#include "CheckpointTransfer.hxx"
#include "SessionRecovery.hxx"
#include <array>
#include <deque>
#include <map>
#include <nlohmann/json_fwd.hpp>

namespace sf4e { namespace session {

// Exact retry coalescing is restricted to idempotent room actions whose
// immutable payload bytes and authenticated sender both match.
bool SameQueuedRoomRetry(const Message& queued, const Message& incoming);

// One game-thread-owned room router per helper. Rust owns encrypted transport;
// SessionServer owns member ordering, permissions, settings and readiness.
class IrohRoom : public std::enable_shared_from_this<IrohRoom> {
public:
	// Degraded means the reliable room/control stream is gone. Existing
	// capability-authorized gameplay links remain pollable and tear down through
	// their own generation, but no new match coordination is admitted.
	enum class State { Idle, Hosting, Joining, Ready, Degraded, Closing, Failed };
	enum class GameState { Preparing, Waiting, Ready, Closing, Closed };
	struct GameSnapshot {
		GameState state = GameState::Closed;
		std::uint64_t generation = 0;
		std::uint16_t virtualPort = 0;
		std::size_t maxPacket = 0;
		std::uint64_t sentPackets = 0, receivedPackets = 0, sentBytes = 0, receivedBytes = 0;
		std::uint64_t rejectedPackets = 0, congestionEvents = 0, localDrops = 0;
        std::string route;
	};
    struct CoordinationSnapshot {
        bool active=false, writable=false, leaderLocal=false, rebound=false;
        std::uint64_t term=0, revision=0, incarnation=0;
        std::size_t voterCount=0, learnerCount=0;
        std::string leader;
    };
    struct CommittedCheckpoint {
        coordination::TransferIdentity identity;
        nlohmann::json checkpoint;
    };
    struct ProbeSnapshot {
        std::uint64_t request=0, pairRevision=0, p95RttUs=0;
        std::string peer, route, status;
        unsigned samples=0, lost=0;
        int recommended=-1;
    };
    struct RecoverySnapshot {
        bool checkpointActive=false, checkpointComplete=false;
        std::uint64_t checkpointTransfer=0, checkpointTerm=0, checkpointRevision=0;
        std::size_t checkpointOffset=0, checkpointLength=0, stagedCheckpoints=0;
        bool pendingCheckpointAck=false, pendingCommittedMarker=false;
        std::uint64_t checkpointRestarts=0, checkpointTimeouts=0, checkpointTransferErrors=0;
        std::uint64_t proposalTransfer=0, proposalTerm=0, proposalBaseRevision=0, proposalElapsedMs=0;
        std::uint64_t proposalTimeouts=0, proposalTransferErrors=0;
        std::size_t proposalBytes=0, proposalSent=0, proposalAcknowledged=0;
        bool proposalBegun=false, proposalEnded=false;
        std::string proposalStatus;
        std::size_t serverQueueMessages=0, clientQueueMessages=0, queuedBytes=0;
        std::size_t pendingPublicEffects=0, pendingTerminalReplays=0, terminalReplaysInFlight=0;
        bool clientHeadCommitted=false;
        std::uint64_t clientHeadTerm=0, clientHeadRevision=0, clientHeadSequence=0;
        room::MemberId clientHeadRecipient=0;
        std::string clientHeadType;
    };
	explicit IrohRoom(platform::HelperClient& helper) : helper_(helper) {}
	bool Host(const std::string& build);
	bool Join(const std::string& invitation, const std::string& build);
	void Leave(bool abandon = false);
	// Fatal room control cannot acknowledge a normal Leave/result. Once native
	// GGPO releases its socket, retire the entire helper epoch and await room_closed.
	bool CloseFailedRoom(bool ggpoOwnsSocket);
	void Poll();
	State GetState() const { return state_; }
	const std::string& Invitation() const { return invitation_; }
	const std::string& DiscordInvitation() const { return discordInvitation_; }
	const std::string& Error() const { return error_; }
	std::uint64_t Epoch() const { return epoch_; }
	std::array<std::uint8_t, 16> RoomId() const { return room_; }
	std::string PeerIdentity(Connection connection) const;
	std::uint64_t PeerIncarnation(Connection connection) const;
	const std::string& LocalIdentity() const { return localIdentity_; }
    const CoordinationSnapshot& Coordination() const { return coordination_; }
    const ProbeSnapshot& Probe() const { return probe_; }
    RecoverySnapshot RecoveryState() const;
    bool ProposeCheckpoint(std::uint64_t request, std::uint64_t term,
        std::uint64_t baseRevision, const nlohmann::json& checkpoint);
    bool TakeCommittedCheckpoint(CommittedCheckpoint& checkpoint);
    // A received checkpoint is only a staged wire record.  The recovery
    // bridge calls this after the native room import and exact connection
    // rebind both succeed; until then client effects remain withheld.
    bool ActivateCommittedCheckpoint(const coordination::TransferIdentity& identity);
    bool ReadyForMatch() const;
    bool ProposalInFlight() const { return !proposalBytes_.empty(); }
    bool RequestProbe(const std::string& peer, std::uint64_t request, std::uint64_t pairRevision);
    Connection ConnectionForIdentity(const std::string& identity) const;
    std::map<Connection,std::string> ControlIdentities() const;
	// The C++ room authority supplies admission and a fresh pair capability.
	// These methods do not infer permission from a transport connection.
	bool PrepareGame(const std::string& peer, std::uint64_t generation,
		const std::array<std::uint8_t, 32>& capability, std::uint16_t localPort,
		std::size_t maxPacket, bool dial);
	// Remove one authorized gameplay edge without ending the generation's other
	// links. This is used when the room authority retires a spectator.
	bool EndPeer(const std::string& peer, std::uint64_t generation);
	bool EndMatch(std::uint64_t generation);
	GameSnapshot Game(const std::string& peer) const;
	std::unique_ptr<ServerTransport> Server();
	std::unique_ptr<ClientTransport> Client();

private:
	class ServerAdapter;
	class ClientAdapter;
	struct Peer {
		std::string identity;
		bool admitted = false;
		std::int64_t nextId = 2;
		std::int64_t receivedId = 1;
	};
	std::map<std::string, std::uint64_t> memberIncarnations_;
	std::set<std::string> committedMembers_;
	bool haveCommittedMembers_ = false;
	void PruneRetiredPeers();
	bool Begin(bool host);
	bool Command(const std::string& payload);
	SendResult SendRemote(Connection connection, const std::string& payload, std::int64_t* id);
	bool Queue(std::deque<Message>& destination, Message message);
	void Fail(const char* code);
	bool ConsumeGameEvent(const nlohmann::json& event, const std::string& type);
    bool ConsumeCoordinationEvent(const nlohmann::json& event, const std::string& type);
    void PumpCheckpoint();
    int AuthorizedEffect(Message& message);
    void PumpCommittedEffects();
    struct StagedCheckpoint {
        coordination::TransferIdentity identity;
        nlohmann::json checkpoint;
        std::vector<EffectEnvelope> effects;
        std::map<room::MemberId,room::ConnectionRef> recipients;
        std::set<std::string> members;
    };
    // A terminal replay is keyed by the immutable recipient identity, rather
    // than a transient helper connection.  This lets a later checkpoint
    // deliver only the still-pending half of a receipt while retaining the
    // exact endpoint/incarnation fence across control reconnects.
    struct TerminalReplayKey {
        std::uint8_t table = 0;
        std::uint64_t generation = 0;
        room::MemberId member = 0;
        room::ConnectionRef endpoint;
        std::uint64_t incarnation = 0;
        std::string type;
        bool operator==(const TerminalReplayKey& rhs) const {
            return table==rhs.table && generation==rhs.generation && member==rhs.member &&
                endpoint==rhs.endpoint && incarnation==rhs.incarnation && type==rhs.type;
        }
        bool operator<(const TerminalReplayKey& rhs) const {
            if (table != rhs.table) return table < rhs.table;
            if (generation != rhs.generation) return generation < rhs.generation;
            if (member != rhs.member) return member < rhs.member;
            if (!(endpoint == rhs.endpoint)) return endpoint < rhs.endpoint;
            if (incarnation != rhs.incarnation) return incarnation < rhs.incarnation;
            return type < rhs.type;
        }
    };
    // Receipt replays are locally derived from an already applied checkpoint.
    // They deliberately do not use EffectEnvelope sequence/term space: those
    // values belong to the replicated journal and the next leader may reuse
    // neither the synthetic sequence nor the old commit term.
    struct TerminalReplayEffect {
        TerminalReplayKey key;
        std::uint64_t roomEpoch = 0;
        std::uint64_t generation = 0;
        std::uint64_t activationRevision = 0;
        room::MemberId recipient = 0;
        room::ConnectionRef endpoint;
        std::string type;
        nlohmann::json publicPayload = nullptr;
        EffectDigest payloadDigest;
    };
    bool BuildTerminalReplayEffects(const StagedCheckpoint& staged,
        const coordination::TransferIdentity& identity,
        std::vector<TerminalReplayEffect>& effects,
        std::set<TerminalReplayKey>& activeKeys) const;
	platform::HelperClient& helper_; // Supervisor outlives this room and its adapters.
	State state_ = State::Idle;
	bool hosting_ = false;
	bool roomCommandQueued_ = false;
	bool leavePending_ = false;
    bool leaveAbandon_=false;
    std::uint64_t leaveRetryAt_=0;
	bool localOpen_ = false;
	bool pendingAdmission_ = false;
	bool serverOpen_ = false;
	std::uint64_t epoch_ = 0;
	Connection nextConnection_ = 2; // 1 is the hosting player's in-process client.
	std::int64_t localNextId_ = 2;
	std::int64_t serverLocalNextId_ = 2;
	std::array<std::uint8_t, 16> room_ = {};
	std::string invitation_, discordInvitation_;
	std::string error_;
	std::string localIdentity_;
	std::uint64_t closedGeneration_ = 0;
	std::map<std::string, GameSnapshot> games_;
	std::map<Connection, Peer> peers_;
	std::deque<Message> serverMessages_;
	std::deque<Message> clientMessages_;
	std::vector<Connection> closed_;
	std::size_t queuedBytes_ = 0;
    CoordinationSnapshot coordination_;
    ProbeSnapshot probe_;
    coordination::CheckpointReceiver checkpointReceiver_;
    std::uint64_t checkpointRestarts_=0, checkpointTimeouts_=0, checkpointTransferErrors_=0;
    std::deque<StagedCheckpoint> committedCheckpoints_;
    coordination::TransferIdentity proposalIdentity_;
    std::string proposalBytes_;
    std::size_t proposalSent_=0, proposalAcknowledged_=0;
    bool proposalBegun_=false, proposalEnded_=false;
    std::uint64_t proposalStartedMs_=0;
    std::uint64_t proposalTimeouts_=0, proposalTransferErrors_=0;
    std::string proposalStatus_;
    nlohmann::json pendingCheckpointAck_;
    nlohmann::json pendingCommittedMarker_;
    std::vector<EffectEnvelope> appliedEffects_;
    std::set<std::pair<std::uint64_t,std::uint64_t>> deliveredEffects_;
    std::map<room::MemberId,room::ConnectionRef> effectRecipients_;
    std::uint64_t effectsRevision_=0;
    std::uint64_t receivedRevision_=0;
    std::deque<EffectEnvelope> pendingPublicEffects_;
    std::deque<TerminalReplayEffect> pendingTerminalReplayEffects_;
    std::map<TerminalReplayKey,TerminalReplayEffect> terminalReplayInFlight_;
    std::set<TerminalReplayKey> deliveredTerminalReplays_;
};

} }
