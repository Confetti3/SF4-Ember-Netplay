#include "IrohRoom.hxx"
#include "IdentityEvents.hxx"
#include <spdlog/spdlog.h>
#include "HelperErrorScope.hxx"
#include "../common/JsonDump.hxx"
#include "../common/InputDelay.hxx"
#include "RoomMessageQueue.hxx"
#include "RoomMessageTag.hxx"
#include "sf4e__SessionProtocol.hxx"
#include "../common/RoomLimits.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>

namespace sf4e { namespace session {
using nlohmann::json;

bool SameQueuedRoomRetry(const Message& queued, const Message& incoming) {
	if (queued.connection != incoming.connection || queued.payload != incoming.payload) return false;
	if (incoming.tag.classified) {
		const auto kind = static_cast<room::ActionKind>(incoming.tag.actionKind);
		return incoming.tag.roomAction && (kind == room::ActionKind::AcknowledgeTerminal ||
			kind == room::ActionKind::RecordResult || kind == room::ActionKind::MatchFinished);
	}
	try {
		const auto decoded = json::parse(incoming.payload).get<SessionProtocol::RoomActionMessage>();
		return decoded.type == SessionProtocol::MT_ROOM_ACTION &&
			(decoded.action.kind == room::ActionKind::AcknowledgeTerminal ||
			 decoded.action.kind == room::ActionKind::RecordResult ||
			 decoded.action.kind == room::ActionKind::MatchFinished);
	} catch (const std::exception&) { return false; }
}

void IrohRoom::Fail(const char* code) {
	// Every room failure names itself once, whether or not the runtime
	// later prints the error it closes the room with.
	if (room_.state != State::Failed) spdlog::warn("Room: failed {} state={}", code, static_cast<int>(room_.state));
	room_.state = State::Failed;
	room_.coordination.writable = false;
	room_.coordination.rebound = false;
	room_.error = code;
	ClearInvitations();
	room_.serverMessages.clear();
	room_.clientMessages.clear();
	room_.queuedBytes = 0;
}

void IrohRoom::CloseAllGames() {
	for (auto& game : room_.games) { game.second.state = GameState::Closed; game.second.virtualPort = 0; }
}

void IrohRoom::ClearInvitations() {
	room_.invitation.clear(); room_.discordInvitation.clear(); room_.shortInvitation.clear(); room_.shortPending = false;
}

void IrohRoom::ReadyIfRebound() {
	if (room_.coordination.writable && room_.coordination.rebound) room_.state = State::Ready;
}

bool IrohRoom::Command(const std::string& payload) {
	if (helper_.Send(payload)) return true;
	Fail("helper_command_queue");
	return false;
}

bool IrohRoom::Begin(bool host) {
	if (room_.state != State::Idle || helper_.State() != platform::HelperState::Connected ||
		epoch_ == (std::numeric_limits<std::uint64_t>::max)()) return false;
	// Room traffic uses raw-body frames, which only a helper of this IPC
	// version reads; one from another build is refused here, not mid-room.
	if (helper_.HelperIpcVersion() != platform::helper_frame::IpcVersion) {
		spdlog::warn("Room: helper IPC version {} differs from {}", helper_.HelperIpcVersion(), platform::helper_frame::IpcVersion);
		return false;
	}
	// The previous epoch's abandon must reach the helper before this epoch's
	// command, or the helper answers the new room with leave_in_progress.
	if (departure_.abandonPending) {
		if (!helper_.Send(json{{"type", "leave"}, {"epoch", departure_.abandonEpoch}, {"abandon", true}}.dump())) return false;
		departure_.abandonPending = false;
	}
	++epoch_;
	// Everything the new epoch owns starts afresh. What outlives an epoch is
	// not in these: the abandon of a departure the helper never confirmed, and
	// the decode tickets, so a previous room's decode still in flight finds no
	// pending entry and is dropped on arrival.
	room_ = EpochState{};
	sender_ = CheckpointSender{};
	replica_ = CommittedReplica{};
	departure_.BeginEpoch();
	room_.hosting = host;
	room_.coordination.active = true;
	decodeOffThread_ = EnvFlag("SF4E_ROOM_WORKER", true);
	latency_.Stop();
	room_.state = host ? State::Hosting : State::Joining;
	return Command("{\"type\":\"status\"}");
}

bool IrohRoom::Host(const std::string& build) {
	if (build.empty() || build.size() > 128) return false;
	if (!Begin(true)) return false;
	departure_.roomCommandQueued = Command(json{{"type", "host"}, {"epoch", epoch_}, {"build", build}}.dump());
	return departure_.roomCommandQueued;
}

bool IrohRoom::RequestShortInvitation() {
	if (!room_.shortInvitation.empty() || room_.shortPending) return true;
	// A failed send is the request's failure, not the room's.
	const bool open = room_.state == State::Ready || room_.state == State::Hosting || room_.state == State::Joining;
	if (!open || room_.invitation.empty() ||
		!helper_.Send(json{{"type", "short_invite"}, {"epoch", epoch_}}.dump())) { ++shortFailures_; return false; }
	room_.shortPending = true;
	return true;
}

bool IrohRoom::Join(const std::string& invitation, const std::string& build, bool observer) {
	if (build.empty() || build.size() > 128) return false;
	if (!Begin(false)) return false;
	json request = {{"type", "join"}, {"epoch", epoch_}, {"invitation", invitation}, {"build", build}};
	// Left out when false, so a plain join is the message every helper reads.
	if (observer) request["observer"] = true;
	departure_.roomCommandQueued = Command(request.dump());
	return departure_.roomCommandQueued;
}

bool IrohRoom::JoinPublic(const std::string& invitation, const json& ticket, const std::string& build) {
	if (build.empty() || build.size() > 128) return false;
	if (!Begin(false)) return false;
	room_.publicMember = true;
	departure_.roomCommandQueued = Command(json{{"type", "join_public"}, {"epoch", epoch_},
		{"invitation", invitation}, {"ticket", ticket}, {"build", build}}.dump());
	return departure_.roomCommandQueued;
}

void IrohRoom::Leave(bool abandon) {
	if (room_.state == State::Idle) return;
    if(room_.state==State::Closing) {
        if(abandon && !departure_.abandon) {departure_.abandon=true; departure_.pending=true; departure_.retryAt=0;}
        return;
    }
    departure_.abandon=abandon;
	room_.state = State::Closing;
    departure_.deadline = GetTickCount64() + LeaveTimeoutMs;

	room_.localOpen = false;
	ClearInvitations();
	room_.serverMessages.clear();
	room_.clientMessages.clear();
	room_.queuedBytes = 0;
	for (auto& game : room_.games) { game.second.state = GameState::Closing; game.second.virtualPort = 0; }
	// A rejected local enqueue never introduced this epoch to the helper.
	// Waiting for a Leave acknowledgment for that epoch would hang cancellation.
	if (!departure_.roomCommandQueued) { room_.state = State::Idle; return; }
	departure_.pending = !helper_.Send(json{{"type", "leave"}, {"epoch", epoch_},{"abandon",departure_.abandon}}.dump());
}

bool IrohRoom::CloseFailedRoom(bool ggpoOwnsSocket) {
	if (room_.state != State::Failed || ggpoOwnsSocket) return false;
	Leave(true);
	return true;
}

const IrohRoom::GameSnapshot* IrohRoom::FindGame(const std::string& peer) const {
	const auto game = room_.games.find(peer);
	return game == room_.games.end() ? nullptr : &game->second;
}

bool IrohRoom::PrepareGame(const std::string& peer, std::uint64_t generation,
	const std::array<std::uint8_t, 32>& capability, std::uint16_t localPort,
	std::size_t maxPacket, bool dial) {
	if (room_.state != State::Ready || !IsEndpointIdentity(peer) || peer == localIdentity_ ||
		generation <= room_.closedGeneration || generation > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
		localPort == 0 || maxPacket == 0 || maxPacket > MaximumUdpPayload ||
		std::all_of(capability.begin(), capability.end(), [](std::uint8_t byte) { return byte == 0; })) return false;
	std::size_t active = 0;
	for (const auto& game : room_.games) {
		if (game.second.state == GameState::Closed) continue;
		if (game.first == peer || game.second.generation != generation) return false;
		++active;
	}
	if (active >= MaximumRemotePeers) return false;
	// Retain closed snapshots until replacement, but never accumulate peers.
	for (auto game = room_.games.begin(); game != room_.games.end();) {
		if (game->second.state == GameState::Closed) game = room_.games.erase(game); else ++game;
	}
	if (!Command(json{{"type", "prepare_game"}, {"epoch", epoch_}, {"peer", peer}, {"room", room_.id},
		{"generation", generation}, {"capability", capability}, {"local_port", localPort},
		{"max_packet", maxPacket}, {"dial", dial}}.dump())) return false;
	GameSnapshot snapshot; snapshot.state = GameState::Preparing; snapshot.generation = generation; snapshot.maxPacket = maxPacket;
	room_.games[peer] = snapshot;
	return true;
}

bool IrohRoom::EndPeer(const std::string& peer, std::uint64_t generation) {
	return EndPeerCommand(peer, generation, true) == PeerEnd::Ended;
}

IrohRoom::PeerEnd IrohRoom::TryEndPeer(const std::string& peer, std::uint64_t generation) {
	return EndPeerCommand(peer, generation, false);
}

IrohRoom::PeerEnd IrohRoom::EndPeerCommand(const std::string& peer, std::uint64_t generation, bool failWhenFull) {
	if ((room_.state != State::Ready && room_.state != State::Degraded) || !IsEndpointIdentity(peer) ||
		generation == 0 || generation <= room_.closedGeneration ||
		generation > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return PeerEnd::Refused;
	auto game = room_.games.find(peer);
	if (game == room_.games.end()) return PeerEnd::Ended;
	if (game->second.generation != generation) return PeerEnd::Refused;
	if (game->second.state == GameState::Closed) {
		room_.games.erase(game);
		return PeerEnd::Ended;
	}
	const auto command = json{{"type", "end_peer"}, {"epoch", epoch_}, {"peer", peer}, {"generation", generation}}.dump();
	if (failWhenFull) { if (!Command(command)) return PeerEnd::Refused; }
	else if (helper_.State() != platform::HelperState::Connected) return PeerEnd::Refused;
	// A connected helper refuses a command only while its queue is full.
	else if (!helper_.Send(command)) return PeerEnd::Busy;
	game->second.state = GameState::Closing;
	game->second.virtualPort = 0;
	return PeerEnd::Ended;
}

bool IrohRoom::EndMatch(std::uint64_t generation) {
	if (generation == 0 || generation <= room_.closedGeneration ||
		(room_.state != State::Ready && room_.state != State::Degraded)) return false;
	// Failed setup can leave no local mapping (or can retire its last edge)
	// before the caller reaches Abort. Preserve the generation fence while
	// making teardown idempotent so the room can return to Idle.
	if (room_.games.empty()) {
		if (!Command(json{{"type", "end_match"}, {"epoch", epoch_}, {"generation", generation}}.dump())) return false;
		room_.closedGeneration = generation;
		return true;
	}
	for (const auto& game : room_.games) if (game.second.generation != generation) return false;
	if (!Command(json{{"type", "end_match"}, {"epoch", epoch_}, {"generation", generation}}.dump())) return false;
	for (const auto& game : room_.games)
		spdlog::info("Room: end_match sent generation={} peer={} state={}", generation, PeerTag(game.first),
			static_cast<int>(game.second.state));
	room_.closedGeneration = generation;
	for (auto& game : room_.games) {
		if (game.second.state != GameState::Closed) game.second.state = GameState::Closing;
		game.second.virtualPort = 0;
	}
	return true;
}

void IrohRoom::AbandonMatch(std::uint64_t generation) {
	for (auto& game : room_.games)
		if (game.second.generation == generation) { game.second.state = GameState::Closed; game.second.virtualPort = 0; }
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
	const auto peer = event.at("peer").get<std::string>();
	const auto generation = event.at("generation").get<std::uint64_t>();
	const auto game = room_.games.find(peer);
	// Lifecycle ends are logged, including ignored ones: a match teardown
	// that never sees its game_closed must show whether it arrived (F-008).
	// The helper reports a peer that closed its end as game_failed
	// (peer_closed) just before game_closed. Every link ends that way after a
	// match, so it is debug here and named on the game_closed line; a loss
	// during play is reported by the match session. Other failures warn.
	if (type == "game_closed" || type == "game_failed") {
		const auto reason = event.value("reason", std::string());
		const bool known = game != room_.games.end() && game->second.generation == generation;
		const auto failure = type == "game_closed" && known ? game->second.error : std::string();
		const auto level = type == "game_closed" ? spdlog::level::info :
			reason == "peer_closed" ? spdlog::level::debug : spdlog::level::warn;
		spdlog::log(level, "Room: {} peer={} generation={} local_generation={} reason={}{}{}", type, PeerTag(peer), generation,
			game == room_.games.end() ? 0 : game->second.generation, reason, failure.empty() ? "" : " failure=", failure);
	}
	if (game == room_.games.end() || generation != game->second.generation) return true;
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
			snapshot.state = GameState::Closed;
			snapshot.virtualPort = 0;
			Fail("invalid_game_mapping");
			return true;
		}
		snapshot.virtualPort = static_cast<std::uint16_t>(port);
		snapshot.route = event.value("route",std::string());
		snapshot.state = GameState::Ready;
		spdlog::info("Room: game_ready peer={} generation={} route={} fixed_port={}", PeerTag(peer), generation,
			RouteLabel(ClassifyRoute(snapshot.route)), event.at("fixed_port").get<bool>() ? "yes" : "no");
	} else if (type == "statistics") {
        snapshot.ObserveStatistics(event);
	}
	return true;
}

IrohRoom::RecoverySnapshot IrohRoom::RecoveryState() const {
    RecoverySnapshot state;
    state.checkpointActive=replica_.checkpointReceiver.Active();
    state.checkpointComplete=replica_.checkpointReceiver.Complete();
    if(state.checkpointActive) {
        const auto& identity=replica_.checkpointReceiver.Identity();
        state.checkpointTransfer=identity.transfer;
        state.checkpointTerm=identity.term;
        state.checkpointRevision=identity.revision;
        state.checkpointOffset=replica_.checkpointReceiver.Offset();
        state.checkpointLength=identity.length;
    }
    state.stagedCheckpoints=replica_.committedCheckpoints.size()+replica_.decoding.size();
    state.pendingCheckpointAck=!replica_.pendingCheckpointAck.is_null();
    state.pendingCommittedMarker=!replica_.pendingCommittedMarker.is_null();
    state.checkpointRestarts=replica_.checkpointRestarts;
    state.checkpointTimeouts=replica_.checkpointTimeouts;
    state.checkpointTransferErrors=replica_.checkpointTransferErrors;
    state.proposalTransfer=sender_.identity.transfer;
    state.proposalTerm=sender_.identity.term;
    state.proposalBaseRevision=sender_.identity.baseRevision;
    state.proposalElapsedMs=sender_.bytes.empty() ? 0 : GetTickCount64()-sender_.startedMs;
    state.proposalTimeouts=sender_.timeouts;
    state.proposalTransferErrors=sender_.transferErrors;
    state.proposalBytes=sender_.bytes.size();
    state.proposalSent=sender_.sent;
    state.proposalAcknowledged=sender_.acknowledged;
    state.proposalBegun=sender_.begun;
    state.proposalEnded=sender_.ended;
    state.proposalStatus=sender_.status;
    state.serverQueueMessages=room_.serverMessages.size();
    state.clientQueueMessages=room_.clientMessages.size();
    state.queuedBytes=room_.queuedBytes;
    state.pendingPublicEffects=replica_.pendingPublicEffects.size();
    state.pendingTerminalReplays=replica_.pendingTerminalReplayEffects.size();
    state.terminalReplaysInFlight=replica_.terminalReplayInFlight.size();
    if(room_.clientMessages.empty()) return state;
    try {
        const auto payload=json::parse(room_.clientMessages.front().payload);
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

std::vector<TournamentAnswer> IrohRoom::TakeTournamentAnswers() {
	std::vector<TournamentAnswer> answers(std::make_move_iterator(tournamentAnswers_.begin()),
		std::make_move_iterator(tournamentAnswers_.end()));
	tournamentAnswers_.clear();
	return answers;
}

bool IrohRoom::SendTournament(const std::string& request, std::uint64_t* requestId) {
	if (request.empty() || request.size() > 8192 || request.front() != '{' || !json::accept(request)) return false;
	std::string payload = "{\"type\":\"tournament\",\"request\":" + request + "}";
	const bool sent = helper_.Send(payload, requestId);
	WipeText(payload);
	return sent;
}

bool IrohRoom::RequestProbe(const std::string& peer, std::uint64_t request, std::uint64_t pairRevision, bool benchmark) {
    if (!room_.coordination.writable || !IsEndpointIdentity(peer) || peer==localIdentity_ || !request) return false;
    if (!helper_.Send(json{{"type","probe_request"},{"epoch",epoch_},{"room",room_.id},
        {"peer",peer},{"request",request},{"pair_revision",pairRevision},{"benchmark",benchmark}}.dump())) return false;
    room_.probe={}; room_.probe.peer=peer; room_.probe.request=request; room_.probe.pairRevision=pairRevision; room_.probe.status="checking"; room_.probe.benchmark=benchmark; room_.probe.deadlineMs=GetTickCount64()+(benchmark?50000:25000);
    return true;
}
void IrohRoom::WatchLatency(bool wanted) {
    const auto watch=[&](std::uint64_t view, bool on) {
        return helper_.Send(json{{"type","latency_watch"},{"epoch",epoch_},{"room",room_.id},{"view",view},{"on",on}}.dump());
    };
    if (!wanted || !epoch_ || room_.id==std::array<std::uint8_t,16>{} || (room_.state!=State::Ready && room_.state!=State::Degraded)) {
        if (const auto view=latency_.Stop()) watch(view, false);
        return;
    }
    const auto now=GetTickCount64();
    const auto view=latency_.Due(epoch_, room_.id, now);
    if (view && watch(view, true)) latency_.Sent(view, epoch_, room_.id, now);
}
bool IrohRoom::ConsumeCoordinationEvent(const json& event, const std::string& type,
    std::optional<std::string_view> body) {
    if (type!="coordination_state" && type!="control_rebound" && type!="checkpoint_begin" &&
        type!="checkpoint_chunk" && type!="checkpoint_end" && type!="checkpoint_ack" &&
        type!="checkpoint_committed" && type!="checkpoint_committed_local" && type!="probe_result") return false;
    const auto room=event.at("room").get<std::array<std::uint8_t,16>>();
    if (room_.id==std::array<std::uint8_t,16>{}) room_.id=room;
    if (room!=room_.id) return true;
    const auto acknowledgeCheckpoint=[&](std::size_t offset) {
        const auto& transfer=replica_.checkpointReceiver.Identity();
        replica_.pendingCheckpointAck=json{{"type","checkpoint_ack"},{"epoch",epoch_},{"room",room_.id},
            {"transfer",transfer.transfer},{"offset",offset}};
        if (helper_.Send(replica_.pendingCheckpointAck.dump())) replica_.pendingCheckpointAck=nullptr;
    };
    if (type=="coordination_state") {
        const auto term=event.at("term").get<std::uint64_t>();
        const auto revision=event.at("revision").get<std::uint64_t>();
        if (term<room_.coordination.term || (term==room_.coordination.term && revision<room_.coordination.revision)) return true;
        if (term!=room_.coordination.term) {
            if(!sender_.bytes.empty()) sender_.status="term_changed";
            sender_.bytes.clear(); room_.coordination.rebound=false;
            room_.probe={};
            if(room_.coordination.term) {room_.invitation.clear(); room_.discordInvitation.clear();}
        }
        room_.coordination.active=true; room_.coordination.term=term; room_.coordination.revision=revision;
        room_.coordination.incarnation=event.at("incarnation"); room_.coordination.leader=event.at("leader");
        room_.coordination.writable=event.at("writable"); room_.coordination.leaderLocal=event.at("leader_local");
        room_.coordination.voterCount=event.value("voter_count",std::size_t(0));
        room_.coordination.learnerCount=event.value("learner_count",std::size_t(0));
        if (room_.coordination.writable && !IsEndpointIdentity(room_.coordination.leader)) {
            room_.coordination.writable=false; room_.error="invalid_coordination_leader";
        }
        if (!room_.coordination.writable && room_.state==State::Ready) { room_.state=State::Degraded; room_.probe={}; }
        if (room_.coordination.writable && room_.coordination.rebound) { room_.hosting=room_.coordination.leaderLocal; room_.state=State::Ready; room_.error.clear(); }
        return true;
    }
    if (type=="control_rebound") {
        if (event.at("leader")!=room_.coordination.leader || !room_.coordination.active) return true;
        const auto incarnations=event.at("member_incarnations").get<std::map<std::string,std::uint64_t>>();
        // A server-owned room's host is a coordination node but not a member.
        if(incarnations.size()>room::MaxMembers+(ServerOwned()?1:0)) return true;
        for(const auto& member:incarnations)
            if(!IsEndpointIdentity(member.first) || !member.second) return true;
        for(const auto& member:event.at("members"))
            if(!incarnations.count(member.get<std::string>())) return true;
        room_.memberIncarnations=incarnations;
        if (!ApplyControlRebound(incarnations, event.at("members").get<std::set<std::string>>())) return true;
        room_.hosting=room_.coordination.leaderLocal; room_.coordination.rebound=true;
        if (room_.coordination.writable) { room_.state=State::Ready; room_.error.clear(); }
        return true;
    }
    if (type=="checkpoint_ack") {
        if (event.at("transfer")!=sender_.identity.transfer || sender_.bytes.empty()) return true;
        const auto offset=event.at("offset").get<std::size_t>();
        if (offset>=sender_.acknowledged && offset<=sender_.sent) sender_.acknowledged=offset;
        return true;
    }
    if (type=="checkpoint_begin") {
        // A newly elected leader initially retains the last committed state
        // from an older term. The helper's local commit marker authenticates
        // that snapshot; a remote term claim never authorizes its import.
        if (replica_.checkpointReceiver.Active()) {
            bool exact=false;
            try {
                exact=replica_.checkpointReceiver.Identity().Matches(event) &&
                    event.at("length")==replica_.checkpointReceiver.Identity().length &&
                    event.at("digest")==replica_.checkpointReceiver.Identity().digest;
            } catch(const json::exception&) {}
            if(!exact) return true;
            // Any unsent ACK belongs to the sender's previous credit window.
            // A retry Begin resets that window even though the immutable
            // transfer identity is unchanged.
            replica_.pendingCheckpointAck=nullptr;
            // A completed body may be waiting behind the bounded committed
            // staging queue or its final ACK may have been delayed. Keep it
            // pinned and make the sender's retry idempotent.
            // The sender's retry starts with a fresh four-chunk credit window.
            // Do not acknowledge the retained full length at Begin: it is not
            // an in-flight boundary in that retry. Exact replayed chunks below
            // prove their own retained byte ranges and advance credit normally.
            if(replica_.checkpointReceiver.Complete()) return true;
            ++replica_.checkpointRestarts;
        }
        if (!replica_.checkpointReceiver.Begin(event,GetTickCount64())) {
            ++replica_.checkpointTransferErrors;
            room_.error="invalid_checkpoint_begin";
        }
        return true;
    }
    if (type=="checkpoint_chunk" || type=="checkpoint_end") {
        if(replica_.checkpointReceiver.Complete()) {
            const auto acknowledged=replica_.checkpointReceiver.CompleteReplayAcknowledgment(event,type,body);
            if(acknowledged) acknowledgeCheckpoint(*acknowledged);
            else ++replica_.checkpointTransferErrors;
            return true;
        }
        const bool accepted=type=="checkpoint_chunk" ? replica_.checkpointReceiver.Chunk(event,body) : replica_.checkpointReceiver.End(event);
        if (!accepted) {
            ++replica_.checkpointTransferErrors;
            replica_.checkpointReceiver.Reset(); room_.error="invalid_checkpoint_transfer"; return true;
        }
        // An acknowledgment is tiny and bounded. Temporary backpressure must
        // never enter Command(), whose failure is terminal for normal control.
        acknowledgeCheckpoint(replica_.checkpointReceiver.Offset());
        return true;
    }
    if (type=="checkpoint_committed_local") {
        // The helper committed this room's own proposal and did not send it
        // back. The copy proposed here becomes the received body, and the
        // answer is the full length; without that exact copy (a term change,
        // a checkpoint-scope error or the proposal timeout cleared it) the
        // answer 0 asks for the full transfer instead.
        const auto transfer=event.at("transfer").get<std::uint64_t>();
        const bool own=!sender_.bytes.empty() && !replica_.checkpointReceiver.Active() &&
            sender_.identity.Matches(event) && event.at("length")==sender_.identity.length &&
            event.at("digest")==sender_.identity.digest &&
            replica_.checkpointReceiver.Adopt(sender_.identity,sender_.bytes,GetTickCount64());
        replica_.pendingCheckpointAck=json{{"type","checkpoint_ack"},{"epoch",epoch_},{"room",room_.id},
            {"transfer",transfer},{"offset",own ? sender_.identity.length : std::size_t(0)}};
        if (helper_.Send(replica_.pendingCheckpointAck.dump())) replica_.pendingCheckpointAck=nullptr;
        if (!own) return true;
        // From here it is a received transfer whose committed marker arrived.
        return ConsumeCoordinationEvent(event,"checkpoint_committed");
    }
    if (type=="checkpoint_committed") {
        if (!replica_.checkpointReceiver.Complete() || !replica_.checkpointReceiver.Identity().Matches(event) ||
            event.at("digest")!=replica_.checkpointReceiver.Identity().digest) return true;
        if(replica_.checkpointReceiver.Identity().revision<=replica_.receivedRevision) {replica_.checkpointReceiver.Reset(); return true;}
        if (replica_.committedCheckpoints.size()+replica_.decoding.size()>=4) {
            if (replica_.pendingCommittedMarker.is_null()) replica_.pendingCommittedMarker=event;
            return true;
        }
        SubmitCommittedCheckpoint(replica_.checkpointReceiver.Identity());
        return true;
    }
    if (type=="probe_result") {
        if(room_.probe.status=="timed_out") return true;
        if (event.at("request")!=room_.probe.request || event.at("peer")!=room_.probe.peer ||
            event.at("pair_revision")!=room_.probe.pairRevision) return true;
        room_.probe.status=event.at("status"); room_.probe.route=event.at("route");
        room_.probe.samples=event.at("sample_count"); room_.probe.lost=event.at("loss_count"); room_.probe.p95RttUs=event.at("p95_rtt_us");
        if(event.contains("metrics")) { const auto& metrics=event.at("metrics");
            room_.probe.p50RttUs=metrics.value("p50_rtt_us",0ULL); room_.probe.p99RttUs=metrics.value("p99_rtt_us",0ULL);
            room_.probe.jitterUs=metrics.value("jitter_us",0ULL);
            room_.probe.sent=metrics.value("sent",0U); room_.probe.expected=metrics.value("expected",0U); room_.probe.packetBytes=metrics.value("packet_bytes",0U); }
        const unsigned expected=room_.probe.benchmark?600:100;
        const int recommendation=event.at("recommended_delay");
        room_.probe.recommended=(room_.probe.status=="complete" || room_.probe.status=="ready") && room_.probe.samples>=expected*4/5 &&
            room_.probe.samples<=expected && room_.probe.samples+room_.probe.lost==expected && recommendation>=RecommendedInputDelayMinimum && recommendation<=InputDelayWireCeiling ? (std::min)(recommendation,MaximumInputDelay) : -1;
        return true;
    }
    return true;
}

IrohRoom::EffectVerdict IrohRoom::CommitGate(const EffectEnvelope& token) const {
    // An effect in the activated journal is committed whatever its term.
    // That is how a successor's replay reaches this client after a handoff:
    // the replay keeps the term it was committed in, which by then is older
    // than the coordination term.
    const bool activated=token.revision<=replica_.effectsRevision &&
        std::find(replica_.appliedEffects.begin(),replica_.appliedEffects.end(),token)!=replica_.appliedEffects.end();
    if(activated) return EffectVerdict::Deliver;
    // An older term's effect that is not in the activated journal may
    // never commit; drop it rather than park it at the queue head. If it
    // did commit, activation replays its public form.
    if(token.term<room_.coordination.term) return EffectVerdict::Drop;
    // The helper's committed effect stream and its coordination watch are
    // independent.  A private grant can therefore beat the watch which
    // announces the checkpoint that activates its recipient mapping. Keep
    // the bounded client queue parked until that activation either makes
    // the token deliverable or rejects the staged checkpoint; dropping it
    // here strands a valid native grant permanently.
    if(token.term>room_.coordination.term || token.revision>replica_.effectsRevision) return EffectVerdict::Park;
    return EffectVerdict::Drop;
}

IrohRoom::EffectVerdict IrohRoom::DeliverCommitted(Message& message, const EffectEnvelope& token, json& payload) {
    const auto recipient=replica_.effectRecipients.find(token.recipient);
    if(recipient==replica_.effectRecipients.end() || recipient->second.host!=token.endpoint.host ||
        recipient->second.user!=token.endpoint.user ||
        !replica_.deliveredEffects.insert({token.term,token.sequence}).second) return EffectVerdict::Drop;
    message.payload=DumpJson(payload);
    message.body=std::make_shared<json>(std::move(payload));
    return EffectVerdict::Deliver;
}

IrohRoom::EffectVerdict IrohRoom::AuthorizedEffect(Message& message) {
    if(!room_.coordination.active) return EffectVerdict::Deliver;
    try {
        // Parked on an earlier poll: its token was decoded and its digest
        // checked against these same bytes on first sight. Only the
        // activation gate can have changed since; the payload is parsed
        // again once, to deliver it.
        if(message.checkedCommit) {
            const auto token=message.checkedCommit;
            const auto verdict=CommitGate(*token);
            if(verdict!=EffectVerdict::Deliver) return verdict;
            auto payload=json::parse(message.payload);
            payload.erase("_commit");
            return DeliverCommitted(message,*token,payload);
        }
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
            const auto queued=replica_.terminalReplayInFlight.find(key);
            auto deliveredPayload=payload;
            deliveredPayload.erase("_terminal_replay");
            if(queued==replica_.terminalReplayInFlight.end()) return EffectVerdict::Drop;
            if(!room_.coordination.rebound) return EffectVerdict::Park;
            if(
                marker.value("room_epoch",std::uint64_t(0))!=queued->second.roomEpoch ||
                marker.value("activation_revision",std::uint64_t(0))!=queued->second.activationRevision ||
                marker.value("payload_digest",std::string())!=queued->second.payloadDigest ||
                PayloadDigest(deliveredPayload)!=queued->second.payloadDigest)
                return EffectVerdict::Drop;
            if(queued->second.recipient!=key.member || !(queued->second.endpoint==key.endpoint) ||
                queued->second.type!=key.type || !replica_.deliveredTerminalReplays.insert(key).second) return EffectVerdict::Drop;
            payload.erase("_terminal_replay");
            replica_.terminalReplayInFlight.erase(queued);
            message.payload=DumpJson(payload);
            message.body=std::make_shared<json>(std::move(payload));
            return EffectVerdict::Deliver;
        }
        if(!payload.contains("_commit")) {
            const auto messageType=payload.value("type",std::string());
            const bool fromLeader=room_.coordination.writable && PeerIdentity(message.connection)==room_.coordination.leader;
            if(!(fromLeader && ((room_.pendingAdmission && (messageType=="hello_resp" || messageType=="join_rej")) ||
                IsVerificationType(messageType)))) return EffectVerdict::Drop;
            message.body=std::make_shared<json>(std::move(payload));
            return EffectVerdict::Deliver;
        }
        const auto token=payload.at("_commit").get<EffectEnvelope>();
        payload.erase("_commit");
        // The digest is checked on first sight, before the gate, so a corrupt
        // or forged token claiming a future term or revision is dropped now
        // instead of parking the queue. It stays SHA-256 over the canonical
        // dump of the parsed payload without its token.
        if(PayloadDigest(payload)!=token.payloadDigest) return EffectVerdict::Drop;
        const auto verdict=CommitGate(token);
        if(verdict==EffectVerdict::Park) message.checkedCommit=std::make_shared<EffectEnvelope>(token);
        if(verdict!=EffectVerdict::Deliver) return verdict;
        return DeliverCommitted(message,token,payload);
    } catch(const std::exception&) { return EffectVerdict::Drop; }
}

void IrohRoom::PumpCommittedEffects() {
    while(!replica_.pendingTerminalReplayEffects.empty() && room_.clientMessages.size()<MaximumQueuedMessages) {
        auto effect=std::move(replica_.pendingTerminalReplayEffects.front());
        replica_.pendingTerminalReplayEffects.pop_front();
        auto payload=effect.publicPayload;
        payload["_terminal_replay"]={{"table",effect.key.table},{"generation",effect.key.generation},
            {"member",effect.key.member},{"endpoint",effect.key.endpoint},{"incarnation",effect.key.incarnation},
            {"type",effect.key.type},{"room_epoch",effect.roomEpoch},
            {"activation_revision",effect.activationRevision},{"payload_digest",effect.payloadDigest}};
        auto bytes=DumpJson(payload);
        if(bytes.size()>MaximumPayload) { Fail("terminal_replay_payload"); return; }
        if(bytes.size()>MaximumQueuedBytes-room_.queuedBytes) {
            replica_.pendingTerminalReplayEffects.push_front(std::move(effect)); break;
        }
        room_.queuedBytes+=bytes.size();
        replica_.terminalReplayInFlight.emplace(effect.key,effect);
        // Classified from the object it was encoded from; marked as a
        // terminal replay here, where it is made.
        room_.clientMessages.push_back({1,0,std::move(bytes),"",ClassifyRoomMessage(payload)});
    }
    while(!replica_.pendingPublicEffects.empty() && room_.clientMessages.size()<MaximumQueuedMessages) {
        const auto& effect=replica_.pendingPublicEffects.front();
        if(replica_.deliveredEffects.count({effect.term,effect.sequence})) {replica_.pendingPublicEffects.pop_front(); continue;}
        auto payload=effect.publicPayload; payload["_commit"]=EffectCommitToken(effect);
        auto bytes=DumpJson(payload);
        if(bytes.size()>MaximumPayload) {replica_.pendingPublicEffects.pop_front(); continue;}
        if(bytes.size()>MaximumQueuedBytes-room_.queuedBytes) break;
        room_.queuedBytes+=bytes.size();
        room_.clientMessages.push_back({1,0,std::move(bytes),"",ClassifyRoomMessage(payload)});
        replica_.pendingPublicEffects.pop_front();
    }
}

bool IrohRoom::Queue(std::deque<Message>& destination, Message message) {
	// A committed proposal is immutable, so SessionServer temporarily stops
	// draining client intents while the helper obtains quorum. Terminal receipt
	// acknowledgements deliberately retry the same action ID during that wait.
	// Keep one authenticated copy in the server queue instead of allowing exact
	// retries to consume the entire bounded transport queue. Distinct actions,
	// senders, and all server-to-client effects retain their original ordering.
	if (&destination == &room_.serverMessages) {
		if (std::any_of(destination.begin(), destination.end(), [&](const Message& queued) {
			return SameQueuedRoomRetry(queued, message);
		})) return true;
	}
	const auto queued = QueueRoomMessage(destination, room_.queuedBytes, std::move(message), &destination == &room_.serverMessages);
	if (queued == RoomQueueResult::BytesFull) {
		Fail("room_receive_bytes");
		return false;
	}
	if (queued == RoomQueueResult::MessagesFull) {
		Fail(&destination == &room_.serverMessages
			? "room_server_receive_queue" : "room_client_receive_queue");
		return false;
	}
	return true;
}

// Once a minute unless the applied roster is empty: commits made and applied
// here, commits waiting to apply, and how full a room control's send queue got
// (queue_full at its capacity drops room messages).
void IrohRoom::LogLoad(const RoomLoadWindow::Report& report) {
	RoomLoadWindow::Room room;
	if (replica_.haveCommittedMembers) room.liveMembers = replica_.committedLiveMembers;
	room.staged = replica_.committedCheckpoints.size() + replica_.decoding.size();
	room.revision = room_.coordination.revision; room.applied = replica_.effectsRevision;
	room.markerParked = !replica_.pendingCommittedMarker.is_null();
	const auto line = room_.loadWindow.Add(report, GetTickCount64(), room);
	if (!line) return;
	spdlog::info("Room: load over {} s members={} revision={} commits={} applied={} staged={} marker_parked={} "
		"control_queue_peak={} event_queue_free_min={}", line->seconds,
		line->room.liveMembers ? std::to_string(*line->room.liveMembers) : std::string("-"), line->room.revision,
		line->commits, line->applied, line->room.staged, line->room.markerParked,
		line->controlQueuePeak ? std::to_string(*line->controlQueuePeak) : std::string("-"), line->eventQueueFreeMin);
}

// The three long Poll branches. Each returns false when Poll must stop for
// this tick (a failure, or the leader-loss freeze) and true to take the next
// helper event. They run inside Poll's json::exception guard.
bool IrohRoom::HandleHelperError(const json& event) {
	const auto code = event.at("code").get<std::string>();
	// Rust reports the control peer which failed. A transient send failure
	// to a non-leader is peer-scoped: the current committed leader still
	// owns the room journal and can replay the queued effect after that peer
	// reconnects. Only a failure addressed to the committed leader (or an
	// older helper that cannot identify its peer) revokes this room's
	// writable/rebound state.
	const auto failedPeer = event.value("peer", std::string());
	const bool failedPeerIsNotLeader = !failedPeer.empty() && IsEndpointIdentity(failedPeer) &&
		failedPeer != room_.coordination.leader;
	const auto verdict = ClassifyHelperError(code, room_.coordination.active, failedPeerIsNotLeader);
	spdlog::warn("Room: helper error {} scope={} peer={} reason={}", code, HelperErrorScopeName(verdict.scope),
		failedPeer.empty() ? std::string("-") : PeerTag(failedPeer), event.value("reason", std::string("-")));
	switch (verdict.scope) {
	case HelperErrorScope::Probe:
		room_.probe.failureReason=event.value("probe_failure",0U);
		room_.probe.status="unavailable"; room_.probe.recommended=-1; return true;
	case HelperErrorScope::Gameplay:
		room_.error="Gameplay connection failed; room control remains available.";
		return true;
	case HelperErrorScope::Checkpoint:
		++replica_.checkpointTransferErrors;
		if(!sender_.bytes.empty()) { ++sender_.transferErrors; sender_.status=code; }
		sender_.bytes.clear(); sender_.begun=sender_.ended=false;
		room_.error="Room checkpoint transfer will retry."; return true;
	case HelperErrorScope::ControlPeer:
		room_.error = "A room peer is reconnecting; committed room control remains available.";
		return true;
	case HelperErrorScope::ControlLeader:
		room_.coordination.writable=false; room_.coordination.rebound=false; room_.state=State::Degraded;
		room_.error="Room control is reconnecting."; return true;
	case HelperErrorScope::Match:
		// The helper no longer holds that match. Its own deadlines (setup,
		// teardown) end it; the room and its other tables carry on.
		room_.error = "The helper dropped a match; the room stays open.";
		return true;
	case HelperErrorScope::RoomFatal:
		break;
	}
	// Which stage a failed host or join reached; the runtime words it.
	room_.failureStage = FailureStageFromHelper(code, event.value("reason", std::string()));
	room_.failureReason = FailureReasonLabel(code, event.value("reason", std::string()));
	// Only protocol-defined labels may enter UI diagnostics.
	Fail(verdict.codeIsProtocolLabel ? code.c_str() : "helper_room_error");
	return false;
}

NetworkSummary IrohRoom::ReadNetworkReport(const json& event) {
	NetworkSummary report;
	report.reported = true;
	report.relay = NormalizeRelayRegion(event.at("relay").get<std::string>());
	report.relayConnected = event.at("relay_connected").get<bool>();
	report.udp = event.at("udp").get<bool>();
	report.nat = NatClassFromHelper(event.at("nat").get<std::string>());
	report.captivePortal = event.at("captive_portal").get<bool>();
	return report;
}

void IrohRoom::Poll() {
    diag::ScopedTimer timer(diag::OP_ROOM_POLL);
    // Stage whatever finished decoding since the last poll, before anything
    // below looks at the staging queue. Never waits.
    CompleteCommittedCheckpoints();
    if(room_.probe.status=="checking" && GetTickCount64()>=room_.probe.deadlineMs) { room_.probe.status="timed_out";room_.probe.recommended=-1; }
    PruneRetiredPeers();
    ExpireDepartedPeers();
	if (helper_.State() == platform::HelperState::Failed || helper_.State() == platform::HelperState::Stopped) {
		CloseAllGames();
		Fail("helper_unavailable");
		return;
	}
	platform::HelperMessage frame;
	// Teardown must survive temporary command backpressure. Retry on later
	// game ticks while continuing to drain events; never block the game thread.
	if (departure_.pending && GetTickCount64()>=departure_.retryAt &&
        helper_.Send(json{{"type", "leave"}, {"epoch", epoch_},{"abandon",departure_.abandon}}.dump())) departure_.pending = false;
	if (departure_.abandonPending && GetTickCount64() >= departure_.abandonRetryAt) {
		if (helper_.Send(json{{"type", "leave"}, {"epoch", departure_.abandonEpoch}, {"abandon", true}}.dump())) departure_.abandonPending = false;
		else departure_.abandonRetryAt = GetTickCount64() + 500;
	}
	if (room_.state == State::Closing && departure_.deadline && GetTickCount64() >= departure_.deadline) {
		// The helper never confirmed the departure. Tell it to abandon the room
		// so it drops the membership handoff, releases the room and closes its
		// control route; the authority then removes this member through that
		// teardown. Without this the helper kept the room and refused every
		// later Join or Host for it until the game restarted. The abandon is
		// retried on this epoch until the helper accepts it, then the room is
		// given up locally so the player can open or join another.
		if (!departure_.abandon) {
			departure_.abandon = true;
			spdlog::warn("Room: departure not confirmed after {} ms; abandoning the helper room", LeaveTimeoutMs);
			departure_.abandonEpoch = epoch_; departure_.abandonRetryAt = GetTickCount64() + 500;
			departure_.abandonPending = !helper_.Send(json{{"type", "leave"}, {"epoch", departure_.abandonEpoch}, {"abandon", true}}.dump());
		}
		CloseAllGames();
		departure_.roomCommandQueued = false; departure_.pending = false; departure_.deadline = 0;
		room_.state = State::Idle; room_.peers.clear(); room_.closed.clear();
		room_.error = "The room did not confirm your departure. You have left locally.";
		return;
	}
	PumpCheckpoint();
	// Bound dispatch independently of each transport's application-message cap.
	for (int budget = 0; budget < 8 && helper_.TryReceive(frame); ++budget) {
		try {
			// A raw-body frame is a JSON header plus the chunk or message body.
			std::size_t headerSize = 0;
			const auto kind = platform::helper_frame::SplitPayload(frame.payload, headerSize);
			if (kind == platform::helper_frame::PayloadKind::Invalid) { Fail("invalid_helper_event"); return; }
			const char* const header = kind == platform::helper_frame::PayloadKind::Raw ?
				frame.payload.data() + platform::helper_frame::RawPrefix : nullptr;
			const auto event = kind == platform::helper_frame::PayloadKind::Raw ?
				json::parse(header, header + headerSize) : json::parse(frame.payload);
			const auto type = event.at("type").get<std::string>();
			std::optional<std::string_view> body;
			if (kind == platform::helper_frame::PayloadKind::Raw) {
				if (type != "checkpoint_chunk" && type != "message") { Fail("invalid_helper_event"); return; }
				body = std::string_view(header + headerSize, frame.payload.size() - platform::helper_frame::RawPrefix - headerSize);
			}
			helperLoad_.lastEventMs = GetTickCount64(); helperLoad_.lastEventType = type;
			if (type == "helper_stall") {
				spdlog::warn("Helper: actor {} {} after {} ms", event.at("ended").get<bool>() ? "resumed from" : "stuck in",
					event.at("stage").get<std::string>(), event.at("stalled_ms").get<std::uint64_t>());
				continue;
			}
			if (type == "stopped") {
				CloseAllGames();
				Fail("helper_stopped");
				return;
			}
			if (type == "status") {
				const auto identity = event.at("endpoint").get<std::string>();
				if (!IsEndpointIdentity(identity)) { Fail("invalid_helper_identity"); return; }
				localIdentity_ = identity;
				localUdpPort_ = event.at("fixed_port").get<std::uint16_t>();
				spdlog::info("Room: helper UDP port {}", *localUdpPort_ ? std::to_string(*localUdpPort_) : "random");
				continue;
			}
			if (type == "network_report") {
				// Endpoint-level, so it has no epoch.
				auto report = ReadNetworkReport(event);
				if (report != network_)
					spdlog::info("Room: network relay={} connected={} udp={} nat={} captive_portal={}",
						report.relay.empty() ? "-" : report.relay, report.relayConnected, report.udp,
						NatClassLabel(report.nat), report.captivePortal);
				network_ = std::move(report);
				continue;
			}
			if (type == "tournament") {
				// Identity and bridge answers have no epoch either. Play answers
				// go to the runtime; the rest are what the Ember ID screens show.
				if (auto answer = ReadTournamentAnswer(event)) {
					if (tournamentAnswers_.size() >= 64) tournamentAnswers_.pop_front();
					tournamentAnswers_.push_back(std::move(*answer));
					continue;
				}
				ApplyTournamentEvent(event, identity_);
				continue;
			}
			if (event.value("epoch", std::uint64_t(0)) != epoch_) continue;
			if (type == "room_closed") {
				departure_.roomCommandQueued = false; departure_.pending = false; departure_.deadline = 0;
				CloseAllGames();
				room_.state = State::Idle; room_.peers.clear(); room_.closed.clear(); continue;
			}
			if (type == "helper_load") {
				helperLoad_.samples++;
				helperLoad_.actorTickLagMaxUs = event.at("actor_tick_lag_max_us").get<std::uint64_t>();
				helperLoad_.actorTickBodyMaxUs = event.at("actor_tick_body_max_us").get<std::uint64_t>();
				helperLoad_.eventQueueFreeMin = event.at("event_queue_free_min").get<std::uint64_t>();
				LogLoad(RoomLoadWindow::Read(event));
				continue;
			}
			// Gameplay is independent of room health. Its lifecycle and counters
			// remain observable after control_closed, until explicit teardown.
			if (ConsumeGameEvent(event, type)) continue;
            if(room_.state==State::Closing && type=="error") {
                const auto code=event.value("code",std::string());
                if(code=="leave_successor_unconfirmed"||code=="leave_membership_unconfirmed"||code=="leave_membership_failed") {
                    departure_.pending=true; departure_.retryAt=GetTickCount64()+500;
                    room_.error=code=="leave_successor_unconfirmed"?
                        "Waiting for the room successor to confirm the transfer.":
                        "Waiting for the room to confirm your departure.";
                    continue;
                }
            }
			if (room_.state == State::Closing || room_.state == State::Idle || room_.state == State::Failed) continue;
			if (ConsumeCoordinationEvent(event,type,body)) continue;
			if (type == "discord_invite") {
                const auto token = event.at("secret").get<std::string>();
                const bool known=token.compare(0,5,"emd1:")==0 || token.compare(0,5,"emd2:")==0;
                if (token.size()>128 || token.size()<6 || !known) {
                    Fail("invalid_discord_invite");
                    return;
                }
                room_.discordInvitation = token;
                room_.invitation = event.at("invitation").get<std::string>();
            } else if (type == "short_invite") {
				// Only a link to the Ember site is ever shown or copied.
				const auto link = event.at("link").get<std::string>();
				const bool ready = event.at("status").get<std::string>() == "ready" && link.size() <= 64 &&
					link.compare(0, 28, "https://embernetplay.link/j#") == 0;
				room_.shortPending = false;
				if (ready) room_.shortInvitation = link;
				else ++shortFailures_;
				spdlog::info("Room: short link {}", ready ? "ready" : "unavailable");
            } else if (type == "hosted" && room_.hosting && room_.state == State::Hosting) {
				const auto room = event.at("room").get<std::array<std::uint8_t, 16>>();
				if (room != room_.id) room_.localLatency = {};
				room_.id = room;
				ReadLocalLatency(event);
				room_.invitation = event.at("invitation").get<std::string>();
				ReadyIfRebound();
			} else if (type == "connected") {
				if (!HandleConnected(event)) return;
			} else if (type == "member_latency") {
				latency_.Read(event, epoch_, room_.id);
			} else if (type == "message" || type == "control_closed" || type == "peer_departed" || type == "peer_session") {
				if (!HandleControlTraffic(event, type, body)) return;
			} else if (type == "error") {
				if (!HandleHelperError(event)) return;
			}
		} catch (const json::exception&) { Fail("invalid_helper_event"); return; }
	}
}

} }
