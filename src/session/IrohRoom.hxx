#pragma once
#include "../netplay/RoomTransport.hxx"
#include "../netplay/LanRooms.hxx"

#include "SessionTransport.hxx"
#include "../platform/HelperClient.hxx"
#include "CheckpointTransfer.hxx"
#include "SessionRecovery.hxx"
#include "RoomLoadWindow.hxx"
#include "CheckpointDecodeWorker.hxx"
#include "../common/NetworkNat.hxx"
#include "../common/NetworkRoute.hxx"
#include "RoomFailure.hxx"
#include "../common/RoomLimits.hxx"
#include "Admission.hxx"
#include "../netplay/IdentityView.hxx"
#include "TournamentAnswers.hxx"
#include "MemberLatency.hxx"
#include <array>
#include <deque>
#include <map>
#include <optional>
#include <string_view>
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
	// Bound on waiting for a departure to be confirmed before the room is
	// released locally. Generous for a normal confirm, short enough not to
	// read as a hang behind the "Leaving room..." status.
	static constexpr std::uint64_t LeaveTimeoutMs = 8000;
	struct GameSnapshot {
		GameState state = GameState::Closed;
		std::uint64_t generation = 0;
		std::uint16_t virtualPort = 0;
		std::size_t maxPacket = 0;
		std::uint64_t sentPackets = 0, receivedPackets = 0, sentBytes = 0, receivedBytes = 0;
		std::uint64_t rejectedPackets = 0, congestionEvents = 0, localDrops = 0, routeChanges = 0;
        std::string route, error;
        std::size_t datagramLimit=0;
        void ObserveStatistics(const nlohmann::json& event);
	};
    const std::map<std::string, GameSnapshot>& Games() const { return room_.games; }
    // The helper process's load over its last second. The actor's 2 ms tick
    // shares the runtime workers with every bridge task, so its lag is the
    // scheduling delay a gameplay packet can also see. samples is zero until
    // the first report arrives.
    struct HelperLoadSnapshot {
        std::uint64_t samples=0, actorTickLagMaxUs=0, actorTickBodyMaxUs=0, eventQueueFreeMin=0;
        // The last helper event of any kind, to tell a silent helper from a
        // lost reply (F-008).
        std::uint64_t lastEventMs=0;
        std::string lastEventType;
    };
    const HelperLoadSnapshot& HelperLoad() const { return helperLoad_; }
    struct CoordinationSnapshot {
        bool active=false, writable=false, leaderLocal=false, rebound=false;
        std::uint64_t term=0, revision=0, incarnation=0;
        std::size_t voterCount=0, learnerCount=0;
        std::string leader;
    };
    // The head commit as decoded and digest-verified once, at receipt. The
    // pointers stay valid until ActivateCommittedCheckpoint retires that head.
    struct CommittedCheckpoint {
        coordination::TransferIdentity identity;
        const SessionProposal* proposal=nullptr;
        // proposal->checkpoint's journal plus proposal->effects, compacted.
        const std::vector<EffectEnvelope>* journal=nullptr;
    };
    struct ProbeSnapshot {
        std::uint64_t request=0, pairRevision=0, p95RttUs=0, p50RttUs=0, p99RttUs=0, jitterUs=0, deadlineMs=0;
        bool benchmark=false;
        std::string peer, route, status;
        unsigned samples=0, lost=0, sent=0, expected=0, packetBytes=0;
        int recommended=-1;
        unsigned failureReason=0;
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
	// Several methods below are virtual for the test double in
	// IrohMatchSessionTest; the destructor follows them so a subclass is never
	// deleted through this base without running its own.
	virtual ~IrohRoom() = default;
	bool Host(const std::string& build,netplay::RoomKind transport);
	// `observer`: the join is a tournament match's watcher, which the room's
	// helper admits only into a room that names it (the helper's `join` type).
	bool Join(const std::string& invitation, const std::string& build, bool observer = false,netplay::RoomKind kind=netplay::RoomKind::Online,const std::string& lanHost={});
	// Join a server-owned room: like Join, with the bridge's signed ticket as received.
	bool JoinPublic(const std::string& invitation, const nlohmann::json& ticket, const std::string& build);
	// Host a server-owned room under the id the bridge issued its tickets for.
	// Members are admitted by tickets signed with ticketKey under ticketKid;
	// the creator's ticket is the only one admitted while the room is empty.
	bool HostPublic(const std::string& build, const std::string& ticketKey, const std::string& ticketKid,
		const std::string& bridgeId, const std::array<std::uint8_t, 16>& roomId, const std::string& creator);
	// The Ember ID and class a public host's helper admitted this connection
	// under; empty in a private room.
	Admission PeerAdmission(Connection connection) const;
	// Public host only: refuse this Ember ID for the rest of the room.
	bool BanAccount(const std::string& account);
	virtual void Leave(bool abandon = false);
	// Fatal room control cannot acknowledge a normal Leave/result. Once native
	// GGPO releases its socket, retire the entire helper epoch and await room_closed.
	bool CloseFailedRoom(bool ggpoOwnsSocket);
	void Poll();
	State GetState() const { return room_.state; }
	// A server-owned (public) room, as its host or as a member that joined with JoinPublic.
	bool ServerOwned() const { return room_.publicHost || room_.publicMember; }
	// Hidden while the link to the leader is down. They survive that loss: the
	// helper sends a joiner's invitation once, at join, and refreshes it only
	// when the authority changes, which clears them here first.
	const std::string& Invitation() const { return room_.state == State::Degraded ? NoInvitation() : room_.invitation; }
	const std::string& DiscordInvitation() const { return room_.state == State::Degraded ? NoInvitation() : room_.discordInvitation; }
	// The room's short link (https://embernetplay.link/j#...), once asked for.
	// The helper answers each request once: the link, which then stays valid
	// for the room's life, even across a change of leader, or a failure,
	// which bumps ShortInvitationFailures so the caller can share the full
	// invitation instead. Returns false, and counts a failure, when no
	// request could be sent.
	bool RequestShortInvitation();
    void WatchLan(bool wanted);
    void PublishLanRoom(const std::string& name,unsigned members,unsigned capacity);
    netplay::RoomTransport Transport() const { return lan_.transport; }
    bool IsLan() const { return lan_.transport==netplay::RoomTransport::Lan; }
    std::shared_ptr<const netplay::LanRooms> LanRooms() const { return lan_.rooms; }
	const std::string& ShortInvitation() const { return room_.state == State::Degraded ? NoInvitation() : room_.shortInvitation; }
	bool ShortInvitationPending() const { return room_.shortPending; }
	std::uint64_t ShortInvitationFailures() const { return shortFailures_; }
	const std::string& Error() const { return room_.error; }
	std::uint64_t Epoch() const { return epoch_; }
	virtual std::array<std::uint8_t, 16> RoomId() const { return room_.id; }
	std::string PeerIdentity(Connection connection) const;
	std::uint64_t PeerIncarnation(Connection connection) const;
	virtual const std::string& LocalIdentity() const { return localIdentity_; }
	// The helper's fixed UDP port, 0 when the OS chose it, or empty until the
	// helper first reports its status.
	std::optional<std::uint16_t> LocalUdpPort() const { return localUdpPort_; }
	// The local endpoint's network as the helper last reported it: home relay
	// region and connection, and how UDP behaves here. Checking until its first
	// net report; never an address.
	const NetworkSummary& Network() const { return network_; }
	// Reads a helper network_report event. Only a region code and flags survive:
	// a relay string that is not a code reads as "other".
	static NetworkSummary ReadNetworkReport(const nlohmann::json& event);
	// Where the last host or join attempt failed, when the helper said.
	FailureStage Stage() const { return room_.failureStage; }
	// The helper's label for that failure (`refused`, `timeout`, `host_unreachable`,
	// ...), empty when it gave none or one this build does not know. Diagnostics
	// only: the player's wording comes from Stage().
	const std::string& FailureReason() const { return room_.failureReason; }
	// The Ember identity and bridge state from the helper's tournament events.
	// Endpoint-level like the network report: kept with or without a room.
	const netplay::IdentityView& Identity() const { return identity_; }
	// The helper's answers to tournament play requests since the last call.
	std::vector<TournamentAnswer> TakeTournamentAnswers();
	// Sends one tournament request, a JSON object, to the helper. Its answer is
	// a tournament event whose request_id is *requestId. The caller wipes the
	// request text, which may hold a passphrase; this wipes its own copy.
	bool SendTournament(const std::string& request, std::uint64_t* requestId);
    virtual const CoordinationSnapshot& Coordination() const { return room_.coordination; }
    const ProbeSnapshot& Probe() const { return room_.probe; }
    RecoverySnapshot RecoveryState() const;
    // bytes is the encoded proposal (SessionProposal::encoded). Deliberately not
    // an overload on json: json converts to std::string implicitly and throws.
    bool ProposeCheckpointBytes(std::uint64_t request, std::uint64_t term,
        std::uint64_t baseRevision, std::string bytes);
    bool TakeCommittedCheckpoint(CommittedCheckpoint& checkpoint);
    // A received checkpoint is only a staged wire record.  The recovery
    // bridge calls this after the native room import and exact connection
    // rebind both succeed; until then client effects remain withheld.
    bool ActivateCommittedCheckpoint(const coordination::TransferIdentity& identity);
    // Drops the staged head without activating it: a commit that predates
    // this session, which the following commit supersedes.
    bool DiscardCommittedCheckpoint(const coordination::TransferIdentity& identity);
    // Received commits waiting behind the staged head, not counting the head.
    std::size_t CommittedCheckpointsBehindHead() const {
        return replica_.committedCheckpoints.empty() ? 0 : replica_.committedCheckpoints.size() - 1;
    }
    virtual bool ReadyForMatch() const;
    bool ProposalInFlight() const { return !sender_.bytes.empty(); }
    // A received commit is being decoded on the worker thread, or is staged
    // and not yet activated: work that completes without a helper event.
    bool CheckpointPending() const { return !replica_.decoding.empty() || !replica_.committedCheckpoints.empty(); }
    bool RequestProbe(const std::string& peer, std::uint64_t request, std::uint64_t pairRevision, bool benchmark=false);
    Connection ConnectionForIdentity(const std::string& identity) const;
    std::map<Connection,std::string> ControlIdentities() const;
	// The C++ room authority supplies admission and a fresh pair capability.
	// These methods do not infer permission from a transport connection.
	virtual bool PrepareGame(const std::string& peer, std::uint64_t generation,
		const std::array<std::uint8_t, 32>& capability, std::uint16_t localPort,
		std::size_t maxPacket, bool dial);
	// Remove one authorized gameplay edge without ending the generation's other
	// links. This is used when the room authority retires a spectator.
	virtual bool EndPeer(const std::string& peer, std::uint64_t generation);
	// EndPeer that does not fail the room when the helper's command queue is
	// momentarily full: Busy then, and the caller tries again.
	enum class PeerEnd { Ended, Busy, Refused };
	virtual PeerEnd TryEndPeer(const std::string& peer, std::uint64_t generation);
	virtual bool EndMatch(std::uint64_t generation);
	// Stop waiting for game_closed on this generation's mappings. The helper
	// clears them when it runs end_match, so only the lost event is skipped.
	virtual void AbandonMatch(std::uint64_t generation);
	// The mapping for this peer, or null when it has none: read in place, for
	// the match session's reads on every tick. Virtual for the test double.
	virtual const GameSnapshot* FindGame(const std::string& peer) const;
	// A copy of that mapping, or a Closed snapshot when there is none.
	GameSnapshot Game(const std::string& peer) const {
		const auto* game = FindGame(peer);
		return game ? *game : GameSnapshot();
	}
	std::unique_ptr<ServerTransport> Server();
	std::unique_ptr<ClientTransport> Client();
	// This member's latency route for the current room, from the helper's
	// hosted or connected event; none until then and after every Host or Join.
	const LatencyRoute& LocalLatency() const { return room_.localLatency; }
	// The room board's latency subscription: on while `wanted` and the room is
	// open, renewed every few seconds, and off otherwise (MemberLatency.hxx).
	void WatchLatency(bool wanted);
	// The ping to each other member for the active subscription; empty without one.
	const MemberLatencyTable& MemberLatencies() const { return latency_.Table(); }
	// The shared endpoint ID predicate (src/common/EndpointIdentity.hxx).
	static bool IsEndpointIdentity(const std::string& identity);

private:
	class ServerAdapter;
	class ClientAdapter;
	static constexpr std::size_t MaximumPayload = 65536;
	static constexpr std::size_t MaximumQueuedMessages = 64;
	static constexpr std::size_t MaximumQueuedBytes = 4 * 1024 * 1024;
	static constexpr std::size_t MaximumUdpPayload = 65507;
	// Active controls remain bounded by the helper's room capacity. Native
	// Started participants may retain stable handles after leaving the room.
	static constexpr std::size_t MaximumRemotePeers = room::MaxMembers - 1 + room::TableCount * room::MaxMatchParticipants;
	static const std::string& NoInvitation() { static const std::string none; return none; }
	// Short, log-safe form of an endpoint identity.
	static std::string PeerTag(const std::string& peer);
	// One session of a remote endpoint. Its transitions live in
	// IrohRoomPeers.cxx so that a departure, a control reconnect and a new
	// session from the same endpoint agree on which handle owns queued work.
	struct Peer {
		std::string identity;
		// Set by a public host's helper from the admitting ticket.
		Admission admission;
		// The helper's process incarnation this session belongs to; 0 until
		// its Admission was accepted.
		std::uint64_t incarnation = 0;
		// The helper's id for the open control connection, absent while it is
		// closed. A close reported for a superseded connection is ignored.
		std::optional<std::uint64_t> control;
		bool admitted = false;
		std::int64_t nextId = 2;
		std::int64_t receivedId = 1;
		// Set when the control closes or the peer announces its departure,
		// cleared when a control connects. Once it passes, the committed
		// leader retires the seat.
		std::uint64_t departureDeadline = 0;
	};
	// Long enough for a control reconnect after a network blip; short enough
	// that a crashed member's seat is freed while the others are still there.
	static constexpr std::uint64_t DepartureGraceMs = 15000;
	Peer* AllocatePeer(const std::string& identity);
	void ControlOpened(Peer& peer, std::uint64_t control);
	void ControlClosed(Peer& peer, std::uint64_t graceMs);
	std::map<Connection, Peer>::iterator FindPeer(const std::string& identity);
	std::map<Connection, Peer>::iterator RetirePeer(std::map<Connection, Peer>::iterator peer);
	// Drops the peer and any of its intents still queued for the server.
	std::map<Connection, Peer>::iterator ErasePeer(std::map<Connection, Peer>::iterator peer);
	bool PeerControlClosed(Connection connection) const;
	void PruneRetiredPeers();
	void ExpireDepartedPeers();
	bool ApplyControlRebound(const std::map<std::string, std::uint64_t>& incarnations,
		const std::set<std::string>& connected);
	bool BeginPeerSession(std::map<Connection, Peer>::iterator peer, std::uint64_t incarnation);
	bool PeerDeparted(std::map<Connection, Peer>::iterator peer);
	// `body` is the payload of a raw-body helper frame, which `event` then lacks.
	bool ReceiveControlMessage(std::map<Connection, Peer>::iterator peer, const nlohmann::json& event,
		std::optional<std::string_view> body);
	bool Begin(bool host);
	bool Command(const std::string& payload);
	PeerEnd EndPeerCommand(const std::string& peer, std::uint64_t generation, bool failWhenFull);
	SendResult SendRemote(Connection connection, const std::string& payload, std::int64_t* id);
	bool Queue(std::deque<Message>& destination, Message message);
	void Fail(const char* code);
	bool ConsumeGameEvent(const nlohmann::json& event, const std::string& type);
    // `body`, when set, is a raw-body frame's checkpoint chunk.
    bool ConsumeCoordinationEvent(const nlohmann::json& event, const std::string& type,
        std::optional<std::string_view> body = std::nullopt);
    // Poll branches; false stops Poll for this tick.
    bool HandleConnected(const nlohmann::json& event);
    bool HandleControlTraffic(const nlohmann::json& event, const std::string& type,
        std::optional<std::string_view> body = std::nullopt);
    bool HandleHelperError(const nlohmann::json& event);
    void PumpCheckpoint();
    // What the client queue does with the message at its head: deliver it,
    // park it there, or drop it.
    enum class EffectVerdict { Deliver, Park, Drop };
    // Parses a message once, attaches the parsed payload as its body when it
    // delivers, and caches a parked effect's checked token on the message.
    EffectVerdict AuthorizedEffect(Message& message);
    // The activation gate for a committed effect whose digest has been checked.
    EffectVerdict CommitGate(const EffectEnvelope& token) const;
    // Recipient and duplicate checks, then rewrites the payload without its
    // token and attaches `payload` as the body.
    EffectVerdict DeliverCommitted(Message& message, const EffectEnvelope& token, nlohmann::json& payload);
    void PumpCommittedEffects();
    struct StagedCheckpoint {
        coordination::TransferIdentity identity;
        SessionProposal proposal;
        std::vector<EffectEnvelope> effects;
        std::map<room::MemberId,room::ConnectionRef> recipients;
        std::set<std::string> members;
        std::size_t liveMembers=0;
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
	// Applies the latency fields of a hosted or connected event when it has them.
	void ReadLocalLatency(const nlohmann::json& event);
	// Commits whose bytes are being decoded off the game thread, oldest first.
	// They count toward the staging capacity and are staged in this order.
	struct PendingDecode {
		std::uint64_t ticket=0;
		coordination::TransferIdentity identity;
		std::uint64_t previousReceivedRevision=0;
	};
	void SubmitCommittedCheckpoint(const coordination::TransferIdentity& identity);
	void CompleteCommittedCheckpoints();
	void FinishCommittedCheckpoint(CheckpointDecodeWorker::Result&& decoded);
	void StageCommittedCheckpoint(const coordination::TransferIdentity& identity,
		CheckpointDecodeWorker::Result&& decoded);
	void LogLoad(const RoomLoadWindow::Report& report);
	// Every game mapping closed now, its virtual port released.
	void CloseAllGames();
	void ClearInvitations();
	// Ready once the coordination is writable and this member is rebound.
	void ReadyIfRebound();

	platform::HelperClient& helper_; // Supervisor outlives this room and its adapters.

	// One room epoch: everything Begin starts afresh for each Host or Join. A
	// field added here is reset with the rest; what outlives an epoch is kept
	// outside it, below.
	struct EpochState {
		State state = State::Idle;
		bool hosting = false;
		// Hosting a server-owned room: this process has no local client.
		bool publicHost = false;
		// Joined a server-owned room through JoinPublic.
		bool publicMember = false;
		bool localOpen = false;
		bool pendingAdmission = false;
		bool serverOpen = false;
		std::int64_t localNextId = 2;
		std::int64_t serverLocalNextId = 2;
		std::array<std::uint8_t, 16> id = {};
		std::string invitation, discordInvitation;
		std::string shortInvitation;
		bool shortPending = false;
		std::string error;
		LatencyRoute localLatency;
		FailureStage failureStage = FailureStage::Unknown;
		std::string failureReason;
		std::uint64_t closedGeneration = 0;
		std::map<std::string, GameSnapshot> games;
		std::map<Connection, Peer> peers;
		// The authenticated roster from control_rebound: endpoint and incarnation.
		std::map<std::string, std::uint64_t> memberIncarnations;
		std::deque<Message> serverMessages;
		std::deque<Message> clientMessages;
		std::vector<Connection> closed;
		std::size_t queuedBytes = 0;
		CoordinationSnapshot coordination;
		ProbeSnapshot probe;
		RoomLoadWindow loadWindow;
	};
	EpochState room_;

	// The leader's checkpoint proposal on its way to the helper: begun, sent in
	// chunks under a four-chunk credit window, ended once all is acknowledged.
	// Empty bytes mean no proposal is in flight.
	struct CheckpointSender {
		coordination::TransferIdentity identity;
		std::string bytes;
		std::size_t sent = 0, acknowledged = 0;
		bool begun = false, ended = false;
		std::uint64_t startedMs = 0;
		std::uint64_t timeouts = 0, transferErrors = 0;
		std::string status;
	};
	CheckpointSender sender_;

	// The committed-checkpoint side: transfers received from the helper, the
	// commits decoded and staged behind the head, and what the applied commit
	// lets through to the local client.
	struct CommittedReplica {
		coordination::CheckpointReceiver checkpointReceiver;
		std::uint64_t checkpointRestarts = 0, checkpointTimeouts = 0, checkpointTransferErrors = 0;
		std::deque<StagedCheckpoint> committedCheckpoints;
		std::deque<PendingDecode> decoding;
		nlohmann::json pendingCheckpointAck;
		nlohmann::json pendingCommittedMarker;
		std::vector<EffectEnvelope> appliedEffects;
		std::set<std::pair<std::uint64_t, std::uint64_t>> deliveredEffects;
		std::map<room::MemberId, room::ConnectionRef> effectRecipients;
		std::uint64_t effectsRevision = 0;
		std::uint64_t receivedRevision = 0;
		std::deque<EffectEnvelope> pendingPublicEffects;
		std::deque<TerminalReplayEffect> pendingTerminalReplayEffects;
		std::map<TerminalReplayKey, TerminalReplayEffect> terminalReplayInFlight;
		std::set<TerminalReplayKey> deliveredTerminalReplays;
		std::set<std::string> committedMembers;
		bool haveCommittedMembers = false;
		// Active rows in the applied commit's roster: members, not frozen history.
		std::size_t committedLiveMembers = 0;
	};
	CommittedReplica replica_;

	// Leaving the room. The first five are this epoch's departure. The abandon
	// of an epoch whose departure the helper never confirmed outlives it.
	struct Departure {
		// The Host or Join command reached the helper: a Leave waits for its answer.
		bool roomCommandQueued = false;
		// A leave the helper has not accepted yet, retried from retryAt.
		bool pending = false;
		bool abandon = false;
		std::uint64_t retryAt = 0;
		// Departure cannot depend on the helper confirming it. Every retryable
		// leave failure re-arms the send, but only until this deadline; after that
		// the room is released locally so the player is never stuck in Closing.
		std::uint64_t deadline = 0;
		// A departure the helper never confirmed is abandoned at that deadline on
		// its own epoch. The abandon is kept until the helper accepts it whatever
		// the room does next; the helper rejects it as stale once a newer epoch
		// has begun, by which point that epoch released the room instead.
		bool abandonPending = false;
		std::uint64_t abandonEpoch = 0, abandonRetryAt = 0;
		void BeginEpoch() { roomCommandQueued = pending = abandon = false; retryAt = deadline = 0; }
	};
	Departure departure_;

	// Outlives every epoch.
	std::uint64_t epoch_ = 0;
	Connection nextConnection_ = 2; // 1 is the hosting player's in-process client.
	std::uint64_t shortFailures_ = 0;
	std::string localIdentity_;
	LatencySubscription latency_;
	std::optional<std::uint16_t> localUdpPort_;
	NetworkSummary network_;
    struct LanState {
        std::shared_ptr<const netplay::LanRooms> rooms = std::make_shared<const netplay::LanRooms>();
        netplay::LanBrowserLease lease;
        std::uint64_t receivedAt = 0, publishAt = 0;
        netplay::RoomTransport transport = netplay::RoomTransport::Online;
        void BeginEpoch() {
            // View ids outlive epochs so delayed discovery events stay stale.
            auto stopped = lease; stopped.Stop();
            *this = LanState{}; lease = stopped;
        }
    } lan_;
	netplay::IdentityView identity_;
	// Bounded: a runtime that stopped draining cannot grow it without limit.
	std::deque<TournamentAnswer> tournamentAnswers_;
	HelperLoadSnapshot helperLoad_;
	std::uint64_t nextDecodeTicket_=1;
	// SF4E_ROOM_WORKER=0 decodes inline on the game thread instead; read at each Begin.
	bool decodeOffThread_=true;
	CheckpointDecodeWorker decoder_;
};

} }
