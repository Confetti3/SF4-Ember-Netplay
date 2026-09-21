#include "IrohRoom.hxx"
#include "RoomMessageQueue.hxx"
#include "sf4e__SessionProtocol.hxx"
#include "../common/RoomLimits.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>

namespace sf4e { namespace session {
// Bound on waiting for the helper to confirm a departure before the room is
// released locally. Generous for a normal confirm, short enough not to read
// as a hang behind the "Leaving room..." status.
static const std::uint64_t kLeaveTimeoutMs = 8000;
using nlohmann::json;
namespace {
constexpr std::size_t MaximumQueuedMessages = 64;
constexpr std::size_t MaximumQueuedBytes = 4 * 1024 * 1024;
constexpr std::size_t MaximumUdpPayload = 65507;
// Active controls remain bounded by the helper's room capacity. Native
// Started participants may retain stable handles after leaving the room.
constexpr std::size_t MaximumRemotePeers = room::MaxMembers - 1 + room::TableCount * room::MaxMatchParticipants;
bool IsEndpointIdentity(const std::string& identity) {
	return identity.size() == 64 && std::all_of(identity.begin(), identity.end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	});
}
}

bool SameQueuedRoomRetry(const Message& queued, const Message& incoming) {
	if (queued.connection != incoming.connection || queued.payload != incoming.payload) return false;
	try {
		const auto decoded = json::parse(incoming.payload).get<SessionProtocol::RoomActionMessage>();
		return decoded.type == SessionProtocol::MT_ROOM_ACTION &&
			(decoded.action.kind == room::ActionKind::AcknowledgeTerminal ||
			 decoded.action.kind == room::ActionKind::RecordResult ||
			 decoded.action.kind == room::ActionKind::MatchFinished);
	} catch (const std::exception&) { return false; }
}

void IrohRoom::Fail(const char* code) {
	state_ = State::Failed;
	coordination_.writable = false;
	coordination_.rebound = false;
	error_ = code;
	invitation_.clear(); discordInvitation_.clear();
	serverMessages_.clear();
	clientMessages_.clear();
	queuedBytes_ = 0;
}

bool IrohRoom::Command(const std::string& payload) {
	if (helper_.Send(payload)) return true;
	Fail("helper_command_queue");
	return false;
}

bool IrohRoom::Begin(bool host) {
	if (state_ != State::Idle || helper_.State() != platform::HelperState::Connected ||
		epoch_ == (std::numeric_limits<std::uint64_t>::max)()) return false;
	++epoch_;
	coordination_ = {}; probe_ = {}; checkpointReceiver_.Reset(); committedCheckpoints_.clear();
	// Decodes still in flight belong to the previous room; with no pending
	// entry left their results are dropped on arrival.
	decoding_.clear();
	decodeOffThread_ = EnvFlag("SF4E_ROOM_WORKER", true);
	checkpointRestarts_=checkpointTimeouts_=checkpointTransferErrors_=0;
    coordination_.active=true;
	proposalBytes_.clear(); proposalIdentity_ = {}; proposalBegun_ = proposalEnded_ = false;
	proposalSent_ = proposalAcknowledged_ = 0;
	proposalTimeouts_ = proposalTransferErrors_ = 0; proposalStatus_.clear();
	pendingCheckpointAck_=nullptr;
    pendingCommittedMarker_=nullptr;
    appliedEffects_.clear(); deliveredEffects_.clear(); effectRecipients_.clear(); effectsRevision_=0; receivedRevision_=0;
    pendingPublicEffects_.clear();
    pendingTerminalReplayEffects_.clear(); terminalReplayInFlight_.clear();
    deliveredTerminalReplays_.clear();
	hosting_ = host;
	roomCommandQueued_ = false;
	leavePending_ = false;
    leaveAbandon_=false;
    leaveRetryAt_=0;
    leaveDeadline_=0;
	localOpen_ = false;
	pendingAdmission_ = false;
	serverOpen_ = false;
	localNextId_ = serverLocalNextId_ = 2;
	peers_.clear();
	memberIncarnations_.clear();
	committedMembers_.clear(); haveCommittedMembers_=false;
	closed_.clear();
	serverMessages_.clear();
	clientMessages_.clear();
	queuedBytes_ = 0;
	room_ = {};
	games_.clear();
	closedGeneration_ = 0;
	invitation_.clear(); discordInvitation_.clear();
	error_.clear();
	state_ = host ? State::Hosting : State::Joining;
	return Command("{\"type\":\"status\"}");
}

bool IrohRoom::Host(const std::string& build) {
	if (build.empty() || build.size() > 128) return false;
	if (!Begin(true)) return false;
	roomCommandQueued_ = Command(json{{"type", "host"}, {"epoch", epoch_}, {"build", build}}.dump());
	return roomCommandQueued_;
}

bool IrohRoom::Join(const std::string& invitation, const std::string& build) {
	if (build.empty() || build.size() > 128) return false;
	if (!Begin(false)) return false;
	roomCommandQueued_ = Command(json{{"type", "join"}, {"epoch", epoch_},
		{"invitation", invitation}, {"build", build}}.dump());
	return roomCommandQueued_;
}

void IrohRoom::Leave(bool abandon) {
	if (state_ == State::Idle) return;
    if(state_==State::Closing) {
        if(abandon && !leaveAbandon_) {leaveAbandon_=true; leavePending_=true; leaveRetryAt_=0;}
        return;
    }
    leaveAbandon_=abandon;
	state_ = State::Closing;
    leaveDeadline_ = GetTickCount64() + kLeaveTimeoutMs;

	localOpen_ = false;
	invitation_.clear(); discordInvitation_.clear();
	serverMessages_.clear();
	clientMessages_.clear();
	queuedBytes_ = 0;
	for (auto& game : games_) { game.second.state = GameState::Closing; game.second.virtualPort = 0; }
	// A rejected local enqueue never introduced this epoch to the helper.
	// Waiting for a Leave acknowledgment for that epoch would hang cancellation.
	if (!roomCommandQueued_) { state_ = State::Idle; return; }
	leavePending_ = !helper_.Send(json{{"type", "leave"}, {"epoch", epoch_},{"abandon",leaveAbandon_}}.dump());
}

bool IrohRoom::CloseFailedRoom(bool ggpoOwnsSocket) {
	if (state_ != State::Failed || ggpoOwnsSocket) return false;
	Leave(true);
	return true;
}

IrohRoom::GameSnapshot IrohRoom::Game(const std::string& peer) const {
	const auto game = games_.find(peer);
	return game == games_.end() ? GameSnapshot() : game->second;
}

bool IrohRoom::PrepareGame(const std::string& peer, std::uint64_t generation,
	const std::array<std::uint8_t, 32>& capability, std::uint16_t localPort,
	std::size_t maxPacket, bool dial) {
	if (state_ != State::Ready || !IsEndpointIdentity(peer) || peer == localIdentity_ ||
		generation <= closedGeneration_ || generation > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
		localPort == 0 || maxPacket == 0 || maxPacket > MaximumUdpPayload ||
		std::all_of(capability.begin(), capability.end(), [](std::uint8_t byte) { return byte == 0; })) return false;
	std::size_t active = 0;
	for (const auto& game : games_) {
		if (game.second.state == GameState::Closed) continue;
		if (game.first == peer || game.second.generation != generation) return false;
		++active;
	}
	if (active >= MaximumRemotePeers) return false;
	// Retain closed snapshots until replacement, but never accumulate peers.
	for (auto game = games_.begin(); game != games_.end();) {
		if (game->second.state == GameState::Closed) game = games_.erase(game); else ++game;
	}
	if (!Command(json{{"type", "prepare_game"}, {"epoch", epoch_}, {"peer", peer}, {"room", room_},
		{"generation", generation}, {"capability", capability}, {"local_port", localPort},
		{"max_packet", maxPacket}, {"dial", dial}}.dump())) return false;
	GameSnapshot snapshot; snapshot.state = GameState::Preparing; snapshot.generation = generation; snapshot.maxPacket = maxPacket;
	games_[peer] = snapshot;
	return true;
}

bool IrohRoom::EndPeer(const std::string& peer, std::uint64_t generation) {
	if ((state_ != State::Ready && state_ != State::Degraded) || !IsEndpointIdentity(peer) ||
		generation == 0 || generation <= closedGeneration_ ||
		generation > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return false;
	auto game = games_.find(peer);
	if (game == games_.end()) return true;
	if (game->second.generation != generation) return false;
	if (game->second.state == GameState::Closed) {
		games_.erase(game);
		return true;
	}
	if (!Command(json{{"type", "end_peer"}, {"epoch", epoch_}, {"peer", peer},
		{"generation", generation}}.dump())) return false;
	game->second.state = GameState::Closing;
	game->second.virtualPort = 0;
	return true;
}

bool IrohRoom::EndMatch(std::uint64_t generation) {
	if (generation == 0 || generation <= closedGeneration_ ||
		(state_ != State::Ready && state_ != State::Degraded)) return false;
	// Failed setup can leave no local mapping (or can retire its last edge)
	// before the caller reaches Abort. Preserve the generation fence while
	// making teardown idempotent so the room can return to Idle.
	if (games_.empty()) {
		if (!Command(json{{"type", "end_match"}, {"epoch", epoch_}, {"generation", generation}}.dump())) return false;
		closedGeneration_ = generation;
		return true;
	}
	for (const auto& game : games_) if (game.second.generation != generation) return false;
	if (!Command(json{{"type", "end_match"}, {"epoch", epoch_}, {"generation", generation}}.dump())) return false;
	closedGeneration_ = generation;
	for (auto& game : games_) {
		if (game.second.state != GameState::Closed) game.second.state = GameState::Closing;
		game.second.virtualPort = 0;
	}
	return true;
}

void IrohRoom::GameSnapshot::ObserveStatistics(const json& event) {
	sentPackets = event.at("sent_packets").get<std::uint64_t>();
	receivedPackets = event.at("received_packets").get<std::uint64_t>();
	sentBytes = event.at("sent_bytes").get<std::uint64_t>();
	receivedBytes = event.at("received_bytes").get<std::uint64_t>();
	rejectedPackets = event.at("rejected_packets").get<std::uint64_t>();
	congestionEvents = event.at("congestion_events").get<std::uint64_t>();
	localDrops = event.at("local_drops").get<std::uint64_t>();
	const auto currentRoute = event.value("route", std::string());
	if (currentRoute != route) ++routeChanges;
	route = currentRoute;
}

bool IrohRoom::ConsumeGameEvent(const json& event, const std::string& type) {
	if (type != "game_waiting" && type != "game_ready" && type != "game_closed" && type != "statistics" && type != "game_failed") return false;
	const auto game = games_.find(event.at("peer").get<std::string>());
	if (game == games_.end() || event.at("generation").get<std::uint64_t>() != game->second.generation) return true;
	auto& snapshot = game->second;
	if (type == "game_closed") { snapshot.state = GameState::Closed; snapshot.virtualPort = 0; return true; }
	if (snapshot.state == GameState::Closing || snapshot.state == GameState::Closed) return true;
    if(type=="game_failed") {
        const auto reason=event.value("reason",std::string());
        if(reason=="packet_limit" || reason=="datagram_limit" || reason=="send_failed" || reason=="peer_closed" || reason=="local_socket")
            snapshot.error="gameplay_"+reason;
        snapshot.datagramLimit=event.value("max_datagram",std::size_t(0));
        return true;
    }
	if (type == "game_waiting" && snapshot.state == GameState::Preparing) snapshot.state = GameState::Waiting;
	if (type == "game_ready") {
		const auto port = event.at("virtual_port").get<std::uint64_t>();
		if (port == 0 || port > 65535 || event.at("max_packet").get<std::size_t>() != snapshot.maxPacket) {
			snapshot.state = GameState::Closed; snapshot.virtualPort = 0;
			Fail("invalid_game_mapping"); return true;
		}
		snapshot.virtualPort = static_cast<std::uint16_t>(port);
		snapshot.route = event.value("route",std::string());
		snapshot.state = GameState::Ready;
	} else if (type == "statistics") {
        snapshot.ObserveStatistics(event);
	}
	return true;
}

std::string IrohRoom::PeerIdentity(Connection connection) const {
	if (connection == 1) return localIdentity_;
	const auto peer = peers_.find(connection);
	return peer == peers_.end() ? std::string() : peer->second.identity;
}

IrohRoom::RecoverySnapshot IrohRoom::RecoveryState() const {
    RecoverySnapshot state;
    state.checkpointActive=checkpointReceiver_.Active();
    state.checkpointComplete=checkpointReceiver_.Complete();
    if(state.checkpointActive) {
        const auto& identity=checkpointReceiver_.Identity();
        state.checkpointTransfer=identity.transfer;
        state.checkpointTerm=identity.term;
        state.checkpointRevision=identity.revision;
        state.checkpointOffset=checkpointReceiver_.Offset();
        state.checkpointLength=identity.length;
    }
    state.stagedCheckpoints=committedCheckpoints_.size()+decoding_.size();
    state.pendingCheckpointAck=!pendingCheckpointAck_.is_null();
    state.pendingCommittedMarker=!pendingCommittedMarker_.is_null();
    state.checkpointRestarts=checkpointRestarts_;
    state.checkpointTimeouts=checkpointTimeouts_;
    state.checkpointTransferErrors=checkpointTransferErrors_;
    state.proposalTransfer=proposalIdentity_.transfer;
    state.proposalTerm=proposalIdentity_.term;
    state.proposalBaseRevision=proposalIdentity_.baseRevision;
    state.proposalElapsedMs=proposalBytes_.empty() ? 0 : GetTickCount64()-proposalStartedMs_;
    state.proposalTimeouts=proposalTimeouts_;
    state.proposalTransferErrors=proposalTransferErrors_;
    state.proposalBytes=proposalBytes_.size();
    state.proposalSent=proposalSent_;
    state.proposalAcknowledged=proposalAcknowledged_;
    state.proposalBegun=proposalBegun_;
    state.proposalEnded=proposalEnded_;
    state.proposalStatus=proposalStatus_;
    state.serverQueueMessages=serverMessages_.size();
    state.clientQueueMessages=clientMessages_.size();
    state.queuedBytes=queuedBytes_;
    state.pendingPublicEffects=pendingPublicEffects_.size();
    state.pendingTerminalReplays=pendingTerminalReplayEffects_.size();
    state.terminalReplaysInFlight=terminalReplayInFlight_.size();
    if(clientMessages_.empty()) return state;
    try {
        const auto payload=json::parse(clientMessages_.front().payload);
        state.clientHeadType=payload.value("type",std::string());
        if(payload.contains("_commit")) {
            const auto effect=payload.at("_commit").get<EffectEnvelope>();
            state.clientHeadCommitted=true;
            state.clientHeadTerm=effect.term;
            state.clientHeadRevision=effect.revision;
            state.clientHeadSequence=effect.sequence;
            state.clientHeadRecipient=effect.recipient;
            if(state.clientHeadType.empty()) state.clientHeadType=effect.type;
        }
    } catch(const std::exception&) { state.clientHeadType="invalid"; }
    return state;
}

std::uint64_t IrohRoom::PeerIncarnation(Connection connection) const {
    const auto member=memberIncarnations_.find(PeerIdentity(connection));
    return member==memberIncarnations_.end() ? 0 : member->second;
}

Connection IrohRoom::ConnectionForIdentity(const std::string& identity) const {
    if (identity == localIdentity_ && !identity.empty()) return 1;
    for (const auto& peer : peers_) if (peer.second.identity == identity) return peer.first;
    return 0;
}
std::map<Connection,std::string> IrohRoom::ControlIdentities() const {
    std::map<Connection,std::string> identities;
    if (!localIdentity_.empty()) identities.emplace(1,localIdentity_);
    for (const auto& peer : peers_) identities.emplace(peer.first,peer.second.identity);
    return identities;
}
bool IrohRoom::RequestProbe(const std::string& peer, std::uint64_t request, std::uint64_t pairRevision, bool benchmark) {
    if (!coordination_.writable || !IsEndpointIdentity(peer) || peer==localIdentity_ || !request) return false;
    if (!helper_.Send(json{{"type","probe_request"},{"epoch",epoch_},{"room",room_},
        {"peer",peer},{"request",request},{"pair_revision",pairRevision},{"benchmark",benchmark}}.dump())) return false;
    probe_={}; probe_.peer=peer; probe_.request=request; probe_.pairRevision=pairRevision; probe_.status="checking"; probe_.benchmark=benchmark; probe_.deadlineMs=GetTickCount64()+(benchmark?50000:25000);
    return true;
}
bool IrohRoom::ConsumeCoordinationEvent(const json& event, const std::string& type) {
    if (type!="coordination_state" && type!="control_rebound" && type!="checkpoint_begin" &&
        type!="checkpoint_chunk" && type!="checkpoint_end" && type!="checkpoint_ack" &&
        type!="checkpoint_committed" && type!="probe_result") return false;
    const auto room=event.at("room").get<std::array<std::uint8_t,16>>();
    if (room_==std::array<std::uint8_t,16>{}) room_=room;
    if (room!=room_) return true;
    const auto acknowledgeCheckpoint=[&](std::size_t offset) {
        const auto& transfer=checkpointReceiver_.Identity();
        pendingCheckpointAck_=json{{"type","checkpoint_ack"},{"epoch",epoch_},{"room",room_},
            {"transfer",transfer.transfer},{"offset",offset}};
        if (helper_.Send(pendingCheckpointAck_.dump())) pendingCheckpointAck_=nullptr;
    };
    if (type=="coordination_state") {
        const auto term=event.at("term").get<std::uint64_t>();
        const auto revision=event.at("revision").get<std::uint64_t>();
        if (term<coordination_.term || (term==coordination_.term && revision<coordination_.revision)) return true;
        if (term!=coordination_.term) {
            if(!proposalBytes_.empty()) proposalStatus_="term_changed";
            proposalBytes_.clear(); coordination_.rebound=false;
            probe_={};
            if(coordination_.term) {invitation_.clear(); discordInvitation_.clear();}
        }
        coordination_.active=true; coordination_.term=term; coordination_.revision=revision;
        coordination_.incarnation=event.at("incarnation"); coordination_.leader=event.at("leader");
        coordination_.writable=event.at("writable"); coordination_.leaderLocal=event.at("leader_local");
        coordination_.voterCount=event.value("voter_count",std::size_t(0));
        coordination_.learnerCount=event.value("learner_count",std::size_t(0));
        if (coordination_.writable && !IsEndpointIdentity(coordination_.leader)) {
            coordination_.writable=false; error_="invalid_coordination_leader";
        }
        if (!coordination_.writable && state_==State::Ready) { state_=State::Degraded; probe_={}; }
        if (coordination_.writable && coordination_.rebound) { hosting_=coordination_.leaderLocal; state_=State::Ready; error_.clear(); }
        return true;
    }
    if (type=="control_rebound") {
        if (event.at("leader")!=coordination_.leader || !coordination_.active) return true;
        const auto incarnations=event.at("member_incarnations").get<std::map<std::string,std::uint64_t>>();
        if(incarnations.size()>room::MaxMembers) return true;
        for(const auto& member:incarnations)
            if(!IsEndpointIdentity(member.first) || !member.second) return true;
        for(const auto& member:event.at("members"))
            if(!incarnations.count(member.get<std::string>())) return true;
        memberIncarnations_=incarnations;
        for(auto& peer:peers_) if(incarnations.count(peer.second.identity)) peer.second.admitted=true;
        PruneRetiredPeers();
        // control_rebound.members describes currently connected control
        // edges, while member_incarnations is the authenticated room roster.
        // Recovery rebind needs a stable logical connection for every admitted
        // endpoint, including a follower that has only its leader edge after
        // takeover. Allocate those handles locally; this does not claim a
        // transport path or synthesize a peer connection.
        for (const auto& entry : incarnations) {
            const auto& identity=entry.first;
            if (identity==localIdentity_) continue;
            if (!IsEndpointIdentity(identity)) return true;
            if (ConnectionForIdentity(identity)) continue;
            if (peers_.size()>=MaximumRemotePeers || nextConnection_==UINT64_MAX) return true;
            Peer peer; peer.identity=identity; peer.admitted=true; peers_.emplace(nextConnection_++,std::move(peer));
        }
        hosting_=coordination_.leaderLocal; coordination_.rebound=true;
        if (coordination_.writable) { state_=State::Ready; error_.clear(); }
        return true;
    }
    if (type=="checkpoint_ack") {
        if (event.at("transfer")!=proposalIdentity_.transfer || proposalBytes_.empty()) return true;
        const auto offset=event.at("offset").get<std::size_t>();
        if (offset>=proposalAcknowledged_ && offset<=proposalSent_) proposalAcknowledged_=offset;
        return true;
    }
    if (type=="checkpoint_begin") {
        // A newly elected leader initially retains the last committed state
        // from an older term. The helper's local commit marker authenticates
        // that snapshot; a remote term claim never authorizes its import.
        if (checkpointReceiver_.Active()) {
            bool exact=false;
            try {
                exact=checkpointReceiver_.Identity().Matches(event) &&
                    event.at("length")==checkpointReceiver_.Identity().length &&
                    event.at("digest")==checkpointReceiver_.Identity().digest;
            } catch(const json::exception&) {}
            if(!exact) return true;
            // Any unsent ACK belongs to the sender's previous credit window.
            // A retry Begin resets that window even though the immutable
            // transfer identity is unchanged.
            pendingCheckpointAck_=nullptr;
            // A completed body may be waiting behind the bounded committed
            // staging queue or its final ACK may have been delayed. Keep it
            // pinned and make the sender's retry idempotent.
            // The sender's retry starts with a fresh four-chunk credit window.
            // Do not acknowledge the retained full length at Begin: it is not
            // an in-flight boundary in that retry. Exact replayed chunks below
            // prove their own retained byte ranges and advance credit normally.
            if(checkpointReceiver_.Complete()) return true;
            ++checkpointRestarts_;
        }
        if (!checkpointReceiver_.Begin(event,GetTickCount64())) {
            ++checkpointTransferErrors_;
            error_="invalid_checkpoint_begin";
        }
        return true;
    }
    if (type=="checkpoint_chunk" || type=="checkpoint_end") {
        if(checkpointReceiver_.Complete()) {
            const auto acknowledged=checkpointReceiver_.CompleteReplayAcknowledgment(event,type);
            if(acknowledged) acknowledgeCheckpoint(*acknowledged);
            else ++checkpointTransferErrors_;
            return true;
        }
        const bool accepted=type=="checkpoint_chunk" ? checkpointReceiver_.Chunk(event) : checkpointReceiver_.End(event);
        if (!accepted) {
            ++checkpointTransferErrors_;
            checkpointReceiver_.Reset(); error_="invalid_checkpoint_transfer"; return true;
        }
        // An acknowledgment is tiny and bounded. Temporary backpressure must
        // never enter Command(), whose failure is terminal for normal control.
        acknowledgeCheckpoint(checkpointReceiver_.Offset());
        return true;
    }
    if (type=="checkpoint_committed") {
        if (!checkpointReceiver_.Complete() || !checkpointReceiver_.Identity().Matches(event) ||
            event.at("digest")!=checkpointReceiver_.Identity().digest) return true;
        if(checkpointReceiver_.Identity().revision<=receivedRevision_) {checkpointReceiver_.Reset(); return true;}
        if (committedCheckpoints_.size()+decoding_.size()>=4) {
            if (pendingCommittedMarker_.is_null()) pendingCommittedMarker_=event;
            return true;
        }
        SubmitCommittedCheckpoint(checkpointReceiver_.Identity());
        return true;
    }
    if (type=="probe_result") {
        if(probe_.status=="timed_out") return true;
        if (event.at("request")!=probe_.request || event.at("peer")!=probe_.peer ||
            event.at("pair_revision")!=probe_.pairRevision) return true;
        probe_.status=event.at("status"); probe_.route=event.at("route");
        probe_.samples=event.at("sample_count"); probe_.lost=event.at("loss_count"); probe_.p95RttUs=event.at("p95_rtt_us");
        if(event.contains("metrics")) { const auto& metrics=event.at("metrics");
            probe_.p50RttUs=metrics.value("p50_rtt_us",0ULL); probe_.p99RttUs=metrics.value("p99_rtt_us",0ULL);
            probe_.jitterUs=metrics.value("jitter_us",0ULL);
            probe_.sent=metrics.value("sent",0U); probe_.expected=metrics.value("expected",0U); probe_.packetBytes=metrics.value("packet_bytes",0U); }
        const unsigned expected=probe_.benchmark?600:100;
        const int recommendation=event.at("recommended_delay");
        probe_.recommended=(probe_.status=="complete" || probe_.status=="ready") && probe_.samples>=expected*4/5 &&
            probe_.samples<=expected && probe_.samples+probe_.lost==expected && recommendation>=0 && recommendation<=10 ? recommendation : -1;
        return true;
    }
    return true;
}

int IrohRoom::AuthorizedEffect(Message& message) {
    if(!coordination_.active) return 1;
    try {
        auto payload=json::parse(message.payload);
        // Receipt-derived terminal notifications are a local replay namespace.
        // They are created only while activating an authenticated checkpoint,
        // so they must not consume replicated sequence numbers or inherit an
        // old control term.  The marker is removed before ClientAdapter sees
        // the application event, and every field is checked against the
        // in-flight proof retained by this room.
        if(payload.contains("_terminal_replay")) {
            const auto marker=payload.at("_terminal_replay");
            const auto key=TerminalReplayKey{
                marker.at("table").get<std::uint8_t>(), marker.at("generation").get<std::uint64_t>(),
                marker.at("member").get<room::MemberId>(), marker.at("endpoint").get<room::ConnectionRef>(),
                marker.at("incarnation").get<std::uint64_t>(), marker.at("type").get<std::string>()};
            const auto queued=terminalReplayInFlight_.find(key);
            auto deliveredPayload=payload;
            deliveredPayload.erase("_terminal_replay");
            if(queued==terminalReplayInFlight_.end()) return -1;
            if(!coordination_.rebound) return 0;
            if(
                marker.value("room_epoch",std::uint64_t(0))!=queued->second.roomEpoch ||
                marker.value("activation_revision",std::uint64_t(0))!=queued->second.activationRevision ||
                marker.value("payload_digest",std::string())!=queued->second.payloadDigest ||
                PayloadDigest(deliveredPayload)!=queued->second.payloadDigest)
                return -1;
            if(queued->second.recipient!=key.member || !(queued->second.endpoint==key.endpoint) ||
                queued->second.type!=key.type || !deliveredTerminalReplays_.insert(key).second) return -1;
            payload.erase("_terminal_replay");
            terminalReplayInFlight_.erase(queued);
            message.payload=payload.dump(); return 1;
        }
        if(!payload.contains("_commit")) {
            const auto messageType=payload.value("type",std::string());
            const bool fromLeader=coordination_.writable && PeerIdentity(message.connection)==coordination_.leader;
            return fromLeader && ((pendingAdmission_ && (messageType=="hello_resp" || messageType=="join_rej")) ||
                IsVerificationType(messageType)) ? 1 : -1;
        }
        const auto token=payload.at("_commit").get<EffectEnvelope>();
        payload.erase("_commit");
        if(PayloadDigest(payload)!=token.payloadDigest) return -1;
        // An effect in the activated journal is committed whatever its term.
        // That is how a successor's replay reaches this client after a handoff:
        // the replay keeps the term it was committed in, which by then is older
        // than the coordination term.
        const bool activated=token.revision<=effectsRevision_ &&
            std::find(appliedEffects_.begin(),appliedEffects_.end(),token)!=appliedEffects_.end();
        if(!activated) {
            // An older term's effect that is not in the activated journal may
            // never commit; drop it rather than park it at the queue head. If it
            // did commit, activation replays its public form.
            if(token.term<coordination_.term) return -1;
            // The helper's committed effect stream and its coordination watch are
            // independent.  A private grant can therefore beat the watch which
            // announces the checkpoint that activates its recipient mapping. Keep
            // the bounded client queue parked until that activation either makes
            // the token deliverable or rejects the staged checkpoint; dropping it
            // here strands a valid native grant permanently.
            if(token.term>coordination_.term || token.revision>effectsRevision_) return 0;
            return -1;
        }
        const auto recipient=effectRecipients_.find(token.recipient);
        if(recipient==effectRecipients_.end() || recipient->second.host!=token.endpoint.host ||
            recipient->second.user!=token.endpoint.user ||
            !deliveredEffects_.insert({token.term,token.sequence}).second) return -1;
        message.payload=payload.dump(); return 1;
    } catch(const std::exception&) { return -1; }
}

void IrohRoom::PumpCommittedEffects() {
    while(!pendingTerminalReplayEffects_.empty() && clientMessages_.size()<MaximumQueuedMessages) {
        auto effect=std::move(pendingTerminalReplayEffects_.front());
        pendingTerminalReplayEffects_.pop_front();
        auto payload=effect.publicPayload;
        payload["_terminal_replay"]={{"table",effect.key.table},{"generation",effect.key.generation},
            {"member",effect.key.member},{"endpoint",effect.key.endpoint},{"incarnation",effect.key.incarnation},
            {"type",effect.key.type},{"room_epoch",effect.roomEpoch},
            {"activation_revision",effect.activationRevision},{"payload_digest",effect.payloadDigest}};
        auto bytes=payload.dump();
        if(bytes.size()>MaximumPayload) { Fail("terminal_replay_payload"); return; }
        if(bytes.size()>MaximumQueuedBytes-queuedBytes_) {
            pendingTerminalReplayEffects_.push_front(std::move(effect)); break;
        }
        queuedBytes_+=bytes.size();
        terminalReplayInFlight_.emplace(effect.key,effect);
        clientMessages_.push_back({1,0,std::move(bytes),""});
    }
    while(!pendingPublicEffects_.empty() && clientMessages_.size()<MaximumQueuedMessages) {
        const auto& effect=pendingPublicEffects_.front();
        if(deliveredEffects_.count({effect.term,effect.sequence})) {pendingPublicEffects_.pop_front(); continue;}
        auto payload=effect.publicPayload; payload["_commit"]=EffectCommitToken(effect);
        auto bytes=payload.dump();
        if(bytes.size()>MaximumPayload) {pendingPublicEffects_.pop_front(); continue;}
        if(bytes.size()>MaximumQueuedBytes-queuedBytes_) break;
        queuedBytes_+=bytes.size();
        clientMessages_.push_back({1,0,std::move(bytes),""});
        pendingPublicEffects_.pop_front();
    }
}

bool IrohRoom::Queue(std::deque<Message>& destination, Message message) {
	// A committed proposal is immutable, so SessionServer temporarily stops
	// draining client intents while the helper obtains quorum. Terminal receipt
	// acknowledgements deliberately retry the same action ID during that wait.
	// Keep one authenticated copy in the server queue instead of allowing exact
	// retries to consume the entire bounded transport queue. Distinct actions,
	// senders, and all server-to-client effects retain their original ordering.
	if (&destination == &serverMessages_) {
		if (std::any_of(destination.begin(), destination.end(), [&](const Message& queued) {
			return SameQueuedRoomRetry(queued, message);
		})) return true;
	}
	const auto queued = QueueRoomMessage(destination, queuedBytes_, std::move(message), &destination == &serverMessages_);
	if (queued == RoomQueueResult::BytesFull) {
		Fail("room_receive_bytes");
		return false;
	}
	if (queued == RoomQueueResult::MessagesFull) {
		Fail(&destination == &serverMessages_
			? "room_server_receive_queue" : "room_client_receive_queue");
		return false;
	}
	return true;
}

SendResult IrohRoom::SendRemote(Connection connection, const std::string& payload, std::int64_t* id) {
	if (state_ != State::Ready) return SendResult::NotConnected;
	if (payload.empty() || payload.size() > MaximumPayload) return SendResult::InvalidPayload;
	auto peer = peers_.find(connection);
	if (peer == peers_.end()) return SendResult::NotConnected;
	if (peer->second.nextId == (std::numeric_limits<std::int64_t>::max)()) return SendResult::Failed;
	const auto messageId = peer->second.nextId;
	if (!Command(json{{"type", "send"}, {"epoch", epoch_}, {"peer", peer->second.identity},
		{"message_id", messageId}, {"payload", payload}}.dump())) return SendResult::QueueFull;
	++peer->second.nextId;
	if (id) *id = messageId;
	return SendResult::Queued;
}

void IrohRoom::PruneRetiredPeers() {
    // Wait until native application of the committed candidate has queued its
    // final effects. Socket loss alone never retires a stable member mapping.
    if(!haveCommittedMembers_ || !committedCheckpoints_.empty() || !decoding_.empty()) return;
    for(auto peer=peers_.begin();peer!=peers_.end();) {
        const auto& identity=peer->second.identity;
        if(!peer->second.admitted) {++peer;continue;}
        const auto game=games_.find(identity);
        if(memberIncarnations_.count(identity) || committedMembers_.count(identity) ||
            (game!=games_.end() && game->second.state!=GameState::Closed)) {++peer;continue;}
        const auto connection=peer->first;
        for(auto message=serverMessages_.begin();message!=serverMessages_.end();) {
            if(message->connection==connection) {queuedBytes_-=message->payload.size();message=serverMessages_.erase(message);}
            else ++message;
        }
        peer=peers_.erase(peer);
    }
}

void IrohRoom::Poll() {
    diag::ScopedTimer timer(diag::OP_ROOM_POLL);
    // Stage whatever finished decoding since the last poll, before anything
    // below looks at the staging queue. Never waits.
    CompleteCommittedCheckpoints();
    if(probe_.status=="checking" && GetTickCount64()>=probe_.deadlineMs) { probe_.status="timed_out";probe_.recommended=-1; }
    PruneRetiredPeers();
	if (helper_.State() == platform::HelperState::Failed || helper_.State() == platform::HelperState::Stopped) {
		for (auto& game : games_) { game.second.state = GameState::Closed; game.second.virtualPort = 0; }
		Fail("helper_unavailable"); return;
	}
	platform::HelperMessage frame;
	// Teardown must survive temporary command backpressure. Retry on later
	// game ticks while continuing to drain events; never block the game thread.
	if (leavePending_ && GetTickCount64()>=leaveRetryAt_ &&
        helper_.Send(json{{"type", "leave"}, {"epoch", epoch_},{"abandon",leaveAbandon_}}.dump())) leavePending_ = false;
	if (state_ == State::Closing && leaveDeadline_ && GetTickCount64() >= leaveDeadline_) {
		// The helper never confirmed the departure. Give the room up locally so
		// the player can open or join another one; the authority still removes
		// this member through its own connection teardown.
		for (auto& game : games_) { game.second.state = GameState::Closed; game.second.virtualPort = 0; }
		roomCommandQueued_ = false; leavePending_ = false; leaveDeadline_ = 0;
		state_ = State::Idle; peers_.clear(); closed_.clear();
		error_ = "The room did not confirm your departure. You have left locally.";
		return;
	}
	PumpCheckpoint();
	// Bound dispatch independently of each transport's application-message cap.
	for (int budget = 0; budget < 8 && helper_.TryReceive(frame); ++budget) {
		try {
			const auto event = json::parse(frame.payload);
			const auto type = event.at("type").get<std::string>();
			if (type == "stopped") {
				for (auto& game : games_) { game.second.state = GameState::Closed; game.second.virtualPort = 0; }
				Fail("helper_stopped"); return;
			}
			if (type == "status") {
				const auto identity = event.at("endpoint").get<std::string>();
				if (!IsEndpointIdentity(identity)) { Fail("invalid_helper_identity"); return; }
				localIdentity_ = identity;
				continue;
			}
			if (event.value("epoch", std::uint64_t(0)) != epoch_) continue;
			if (type == "room_closed") {
				roomCommandQueued_ = false; leavePending_ = false; leaveDeadline_ = 0;
				for (auto& game : games_) { game.second.state = GameState::Closed; game.second.virtualPort = 0; }
				state_ = State::Idle; peers_.clear(); closed_.clear(); continue;
			}
			if (type == "helper_load") {
				helperLoad_.samples++;
				helperLoad_.actorTickLagMaxUs = event.at("actor_tick_lag_max_us").get<std::uint64_t>();
				helperLoad_.actorTickBodyMaxUs = event.at("actor_tick_body_max_us").get<std::uint64_t>();
				helperLoad_.eventQueueFreeMin = event.at("event_queue_free_min").get<std::uint64_t>();
				continue;
			}
			// Gameplay is independent of room health. Its lifecycle and counters
			// remain observable after control_closed, until explicit teardown.
			if (ConsumeGameEvent(event, type)) continue;
            if(state_==State::Closing && type=="error") {
                const auto code=event.value("code",std::string());
                if(code=="leave_successor_unconfirmed"||code=="leave_membership_unconfirmed"||code=="leave_membership_failed") {
                    leavePending_=true; leaveRetryAt_=GetTickCount64()+500;
                    error_=code=="leave_successor_unconfirmed"?
                        "Waiting for the room successor to confirm the transfer.":
                        "Waiting for the room to confirm your departure.";
                    continue;
                }
            }
			if (state_ == State::Closing || state_ == State::Idle || state_ == State::Failed) continue;
			if (ConsumeCoordinationEvent(event,type)) continue;
			if (type == "discord_invite") {
                const auto token = event.at("secret").get<std::string>();
                if (token.size()>128 || token.size()<6 || (token.compare(0,5,"emd1:") && token.compare(0,5,"emd2:"))) { Fail("invalid_discord_invite"); return; }
                discordInvitation_ = token;
                invitation_ = event.at("invitation").get<std::string>();
            } else if (type == "hosted" && hosting_ && state_ == State::Hosting) {
				room_ = event.at("room").get<std::array<std::uint8_t, 16>>();
				invitation_ = event.at("invitation").get<std::string>();
				if(coordination_.writable && coordination_.rebound) state_=State::Ready;
			} else if (type == "connected") {
				const auto identity = event.at("peer").get<std::string>();
				const auto room = event.at("room").get<std::array<std::uint8_t, 16>>();
				if (hosting_ && room != room_) { Fail("wrong_room"); return; }
				if (!hosting_ && state_ != State::Joining && !coordination_.active) { Fail("unexpected_peer"); return; }
				if (coordination_.active && ConnectionForIdentity(identity)) continue;
				for (const auto& peer : peers_) if (peer.second.identity == identity) {
					Fail("duplicate_peer"); return;
				}
				PruneRetiredPeers();
				if (peers_.size() >= MaximumRemotePeers || nextConnection_ == (std::numeric_limits<Connection>::max)()) {
					Fail("peer_limit"); return;
				}
				Peer peer; peer.identity = identity;
				peer.admitted=memberIncarnations_.count(identity) || committedMembers_.count(identity);
				peers_.emplace(nextConnection_++, std::move(peer));
				room_ = room;
				if(coordination_.writable && coordination_.rebound) state_=State::Ready;
			} else if (type == "message" || type == "control_closed") {
				const auto identity = event.at("peer").get<std::string>();
				auto peer = std::find_if(peers_.begin(), peers_.end(), [&](const std::pair<const Connection, Peer>& item) {
					return item.second.identity == identity;
				});
				if (peer == peers_.end()) continue; // Late event for a departed peer.
				if (type == "control_closed") {
					if (coordination_.active) {
						// A control socket is not a membership decision. Retain its
						// stable mapping while committed coordination reconnects it.
						if (identity==coordination_.leader) {
							coordination_.writable=false; coordination_.rebound=false;
							state_=State::Degraded; invitation_.clear(); discordInvitation_.clear();
                            probe_={};
							// Make the freeze observable for one owner tick even if a
							// queued authority watch and rebound follow immediately. This
							// prevents the UI or SessionServer from issuing a mutation in
							// the same poll that detected loss of its leader control.
							return;
						}
						continue;
					}
					if (closed_.size() >= MaximumQueuedMessages) { Fail("room_close_queue"); return; }
					closed_.push_back(peer->first);
					const auto departed = peer->first;
					peers_.erase(peer);
					for (auto message = serverMessages_.begin(); message != serverMessages_.end();) {
						if (message->connection == departed) {
							queuedBytes_ -= message->payload.size(); message = serverMessages_.erase(message);
						} else ++message;
					}
					if (!hosting_) {
						// Keep the loss visible to the room controller while retaining
						// independently authorized gameplay mappings. New match
						// preparation is blocked by the degraded state.
						state_ = State::Degraded;
                        invitation_.clear(); discordInvitation_.clear();
						error_ = "room_control_closed";
					}
				} else {
					const auto id = event.at("message_id").get<std::int64_t>();
					const auto payload = event.at("payload").get<std::string>();
					if (id <= peer->second.receivedId || payload.empty() || payload.size() > MaximumPayload) {
						Fail("invalid_control_message"); return;
					}
					peer->second.receivedId = id;
					// Authority watches and control streams can arrive in either
					// order during takeover. Committed effects always go through
					// recipient validation; client intents wait behind the server's
					// local committed-leader gate regardless of the current UI role.
                    const auto decoded=json::parse(payload);
                    const bool effect=decoded.is_object() && decoded.contains("_commit");
                    // Only a bare admission response from the committed
                    // leader enters the local client queue.  Other untagged
                    // peer payloads remain server intents.
                    const auto messageType=decoded.value("type",std::string());
                    const bool fromLeader=!effect && coordination_.active && coordination_.writable &&
                        peer->second.identity==coordination_.leader;
                    const bool leaderHandshake=fromLeader && pendingAdmission_ &&
                        (messageType=="hello_resp" || messageType=="join_rej");
                    // Verification is forwarded by the leader without a commit
                    // token; it carries no room mutation, so it is delivered
                    // directly rather than through a checkpoint.
                    const bool leaderVerification=fromLeader && IsVerificationType(messageType);
                    if (!Queue((effect || leaderHandshake || leaderVerification) ? clientMessages_ : serverMessages_, {peer->first, id, payload, ""})) return;
				}
			} else if (type == "error") {
				const auto code = event.at("code").get<std::string>();
                if(code=="probe_unavailable") {
                    probe_.failureReason=event.value("probe_failure",0U);
                    probe_.status="unavailable"; probe_.recommended=-1; continue;
                }
				if(code=="gameplay_prepare_failed") {
					error_="Gameplay connection failed; room control remains available.";
					continue;
				}
                if(coordination_.active && (code.compare(0,11,"checkpoint_")==0 ||
                    code.compare(0,19,"invalid_checkpoint_")==0 ||
                    code=="stale_checkpoint_ack" || code=="unexpected_checkpoint_ack")) {
                    ++checkpointTransferErrors_;
                    if(!proposalBytes_.empty()) { ++proposalTransferErrors_; proposalStatus_=code; }
                    proposalBytes_.clear(); proposalBegun_=proposalEnded_=false;
                    error_="Room checkpoint transfer will retry."; continue;
                }
                if(coordination_.active && (code=="control_send_failed" || code=="coordination_unavailable")) {
                    // Rust reports the control peer which failed.  A transient
                    // send failure to a non-leader is peer-scoped: the current
                    // committed leader still owns the room journal and can
                    // replay the queued effect after that peer reconnects.
                    // Only a failure addressed to the committed leader (or an
                    // older helper that cannot identify its peer) revokes this
                    // room's writable/rebound state.
                    const auto failedPeer = event.value("peer", std::string());
                    if (!failedPeer.empty() && IsEndpointIdentity(failedPeer) &&
                        failedPeer != coordination_.leader) {
                        error_ = "A room peer is reconnecting; committed room control remains available.";
                        continue;
                    }
                    coordination_.writable=false; coordination_.rebound=false; state_=State::Degraded;
                    invitation_.clear(); discordInvitation_.clear();
                    error_="Room control is reconnecting."; continue;
                }
				// Only protocol-defined labels may enter UI diagnostics.
				if (code == "invalid_or_incompatible_invitation" || code == "join_failed" ||
					code == "host_unavailable" || code == "invalid_room_state" ||
					code == "control_send_failed" || code == "invalid_control_size") Fail(code.c_str());
				else Fail("helper_room_error");
				return;
			}
		} catch (const json::exception&) { Fail("invalid_helper_event"); return; }
	}
}

} }
