#include "SessionController.hxx"

#include <limits>

namespace sf4e { namespace netplay {

namespace {
const size_t MaxInviteLength = 4096;
const size_t MaxErrorLength = 255;
// A commit is announced a few game ticks before its checkpoint applies here.
const std::uint64_t CatchUpGraceMs = 250;
}

Decision SessionController::Accept(Effect effect) const {
    Decision result;
    result.accepted = true;
    result.effect = effect;
    result.generation = state_.generation;
    return result;
}

void SessionController::ResetRoom() {
    // Keep the monotonically increasing room identity across Offline/Leave.
    const Generation generation = state_.generation;
    state_ = Snapshot();
    state_.generation = generation;
}

bool SessionController::ObserveCoordination(std::uint64_t term, std::uint64_t revision,
    bool writable, std::uint64_t nowMs, bool locallyApplied) {
    if (state_.room == RoomState::Idle || state_.room == RoomState::Closing ||
        term < state_.authorityTerm || (term == state_.authorityTerm && revision < state_.authorityRevision)) return false;
    state_.coordinated = true;
    state_.authorityTerm = term;
    state_.authorityRevision = revision;
    // Bulk checkpoint delivery can lag a healthy same-term control stream.
    // Keep mutations fenced until application catches up without treating
    // every ordinary room revision as a disconnect or clearing pending Ready.
    // That ordinary gap stays writable, so controls do not flicker with every
    // commit; the authority still rejects a command made on a stale revision.
    const bool connected = writable && term != 0;
    if (!connected || locallyApplied) state_.authorityStalledMs = 0;
    else if (state_.authorityStalledMs == 0) state_.authorityStalledMs = nowMs ? nowMs : 1;
    state_.authorityWritable = connected && (locallyApplied ||
        (nowMs >= state_.authorityStalledMs && nowMs - state_.authorityStalledMs < CatchUpGraceMs));
    const bool opening = !ControlPlaneEstablished();
    if (connected || !opening) { state_.openingStalledMs = 0; state_.openingStalled = false; }
    if (connected) {
        state_.recovery = Recovery::None;
        state_.recoveryStartedMs = 0;
        if (!opening) {
            state_.room = RoomState::Joined;
            state_.control = Health::Healthy;
        }
        state_.error.clear();
        AdvanceCatchUp(nowMs);
        return true;
    }
    state_.verificationAvailable = false;
    state_.readyPending = false;
    if (opening) {
        // Creating or joining: the coordination stream is still forming, so
        // this is not a lost room and has no control to recover. A slow relay
        // can still finish, so a long wait is named rather than ended; the
        // player can stop it.
        if (state_.openingStalledMs == 0) state_.openingStalledMs = nowMs ? nowMs : 1;
        state_.openingStalled = nowMs >= state_.openingStalledMs && nowMs - state_.openingStalledMs >= OpeningStallMs;
        return true;
    }
    if (state_.recovery == Recovery::None) {
        state_.recovery = Recovery::Recovering;
        state_.recoveryStartedMs = nowMs;
    }
    state_.control = Health::Lost;
    state_.error = "Room control is recovering. Active matches may finish; room actions are paused.";
    AdvanceRecovery(nowMs);
    return true;
}

// A connected same-term stream whose checkpoint never applies locally leaves
// every room mutation fenced with recovery None, no explanation and no way out.
// Ordinary lag clears in well under a second, so a persistent stall is a fault:
// name it, then offer replacement on the same schedule as a lost stream.
void SessionController::AdvanceCatchUp(std::uint64_t nowMs) {
    if (!state_.authorityStalledMs || state_.authorityWritable) return;
    if (nowMs < state_.authorityStalledMs) { state_.authorityStalledMs = nowMs ? nowMs : 1; return; }
    const auto stalled = nowMs - state_.authorityStalledMs;
    if (stalled >= 10000)
        state_.error = "The room is still catching up on this PC. Room actions are paused.";
    if (stalled >= 30000 && state_.recovery == Recovery::None) {
        state_.recovery = Recovery::ReplacementOffered;
        state_.recoveryStartedMs = state_.authorityStalledMs;
    }
}

void SessionController::AdvanceRecovery(std::uint64_t nowMs) {
    if (state_.recovery == Recovery::Recovering && nowMs >= state_.recoveryStartedMs &&
        nowMs - state_.recoveryStartedMs >= 15000) state_.recovery = Recovery::ReplacementOffered;
}

bool SessionController::FencedOut(const Command& command) const {
    return command.generation == state_.generation && state_.coordinated && !state_.authorityWritable &&
        (command.kind == CommandKind::Ready || command.kind == CommandKind::Rematch ||
         command.kind == CommandKind::RoomAction || command.kind == CommandKind::SetLobbySettings ||
         command.kind == CommandKind::CheckConnection || command.kind == CommandKind::ApplyDelay);
}

Decision SessionController::Execute(const Command& command) {
    if (!(command.generation == state_.generation)) { return Decision(); }
    if (FencedOut(command)) { Decision fenced; fenced.refusal = Refusal::Fenced; return fenced; }
    switch (command.kind) {
    case CommandKind::HostRoom:
    case CommandKind::JoinInvite: {
        if (state_.room != RoomState::Idle || state_.match != MatchState::None
            || state_.generation.room == std::numeric_limits<uint64_t>::max()) { return Decision(); }
        const bool host = command.kind == CommandKind::HostRoom;
        if (!host && (command.invitation.empty() || command.invitation.size() > MaxInviteLength)) { return Decision(); }
        ResetRoom();
        ++state_.generation.room;
        state_.generation.match = 0;
        state_.room = RoomState::Opening;
        state_.control = Health::Connecting;
        state_.isHost = host;
        Decision result = Accept(host ? Effect::HostRoom : Effect::JoinInvite);
        if (!host) { result.invitation = command.invitation; }
        return result;
    }
    case CommandKind::LeaveRoom:
        if (state_.room == RoomState::Idle || state_.room == RoomState::Closing) { return Decision(); }
        state_.room = RoomState::Closing;
        state_.readyPending = false;
        // Resource teardown must be acknowledged at a safe game boundary
        // before a new room, Offline, or any update installation may begin.
        return Accept(Effect::CloseSession);
    case CommandKind::ReplaceRoom:
        if (state_.recovery != Recovery::ReplacementOffered || state_.room == RoomState::Closing ||
            state_.match == MatchState::Playing) return Decision();
        state_.room = RoomState::Closing;
        state_.readyPending = false;
        return Accept(Effect::ReplaceRoom);
    case CommandKind::CheckConnection:
    case CommandKind::ApplyDelay:
        if (state_.room != RoomState::Joined || state_.control != Health::Healthy || state_.readyPending ||
            (state_.match != MatchState::None && state_.match != MatchState::PostMatch)) return Decision();
        return Accept(command.kind == CommandKind::CheckConnection ? Effect::CheckConnection : Effect::ApplyDelay);
    case CommandKind::Ready:
    case CommandKind::Rematch:
        if (state_.room != RoomState::Joined || state_.control != Health::Healthy || state_.readyPending
            || (state_.match != MatchState::None && state_.match != MatchState::PostMatch)) { return Decision(); }
        if (command.kind == CommandKind::Rematch && state_.match != MatchState::PostMatch) { return Decision(); }
        state_.readyPending = true;
        return Accept(Effect::SendReady);
    case CommandKind::StartOffline:
        if (state_.room != RoomState::Idle || state_.match != MatchState::None) { return Decision(); }
        state_.error.clear();
        return Accept(Effect::StartOffline);
    case CommandKind::SavePreferences:
        if (state_.room != RoomState::Idle || state_.match != MatchState::None) return Decision();
        return Accept(Effect::SavePreferences);
    case CommandKind::SetLobbySettings:
        if (state_.room != RoomState::Joined || state_.control != Health::Healthy || state_.readyPending ||
            (state_.match != MatchState::None && state_.match != MatchState::PostMatch)) return Decision();
        // The application/room owner additionally verifies P1 and all readiness.
        return Accept(Effect::SetLobbySettings);
    case CommandKind::RequestUpdate:
        // A check/download is asynchronous and harmless during a session.
        // Installation is a separate bootstrap operation after binary unload.
        return Accept(Effect::CheckUpdate);
    case CommandKind::RoomAction:
        if (state_.room != RoomState::Joined || state_.control != Health::Healthy) return Decision();
        // The authority validates table revision, membership, and permission.
        // Chat and room moderation remain available during a local fight.
        return Accept(Effect::SendRoomAction);
    }
    return Decision();
}

Decision SessionController::Apply(const Event& event) {
    if (event.generation.room != state_.generation.room) { return Decision(); }
    // The control connection lives across rematches. Its loss/close events
    // carry room ownership, not whichever match happened to be current when
    // the worker was launched. Match callbacks require both identities.
    const bool roomEvent = event.kind == EventKind::RoomJoined || event.kind == EventKind::RoomFailed
        || event.kind == EventKind::RoomClosed || event.kind == EventKind::ControlLost
        || event.kind == EventKind::HelperLost;
    if (!roomEvent && event.generation.match != state_.generation.match) { return Decision(); }
    if (state_.room == RoomState::Closing && event.kind != EventKind::RoomClosed
        && event.kind != EventKind::HelperLost) { return Decision(); }
    switch (event.kind) {
    case EventKind::RoomJoined:
        if (state_.room != RoomState::Opening) { return Decision(); }
        state_.room = RoomState::Joined;
        state_.control = !state_.coordinated || state_.authorityWritable ? Health::Healthy : Health::Lost;
        return Accept();
    case EventKind::RoomFailed:
        if (state_.room != RoomState::Opening) { return Decision(); }
        state_.room = RoomState::Closing;
        state_.control = Health::Lost;
        state_.error = event.error.substr(0, MaxErrorLength);
        return Accept(Effect::CloseSession);
    case EventKind::RoomClosed: {
        if (state_.room != RoomState::Closing) { return Decision(); }
        const std::string error = state_.error;
        ResetRoom();
        state_.error = error;
        return Accept();
    }
    case EventKind::ReadyAcknowledged:
        if (state_.room != RoomState::Joined || !state_.readyPending) { return Decision(); }
        state_.readyPending = false;
        return Accept();
    case EventKind::MatchPreparing:
        if (state_.room != RoomState::Joined || state_.control != Health::Healthy
            || (state_.match != MatchState::None && state_.match != MatchState::PostMatch)
            || state_.generation.match == std::numeric_limits<uint64_t>::max()) { return Decision(); }
        ++state_.generation.match;
        state_.match = MatchState::Preparing;
        state_.gameplay = Health::Connecting;
        state_.readyPending = false;
        return Accept();
    case EventKind::GameplayReady:
        if (state_.room != RoomState::Joined || state_.match != MatchState::Preparing) { return Decision(); }
        state_.gameplay = Health::Healthy;
        return Accept();
    case EventKind::MatchStarted:
        if (state_.room != RoomState::Joined || state_.match != MatchState::Preparing
            || state_.gameplay != Health::Healthy) { return Decision(); }
        state_.match = MatchState::Playing;
        state_.verificationAvailable = true;
        return Accept();
    case EventKind::MatchEnded:
        // A battle that closes before GGPO ever reached Running (the peer
        // never synchronised) still ends the match; leaving Preparing in
        // place fenced Ready for the rest of the room's life.
        if (state_.match != MatchState::Playing && state_.match != MatchState::Preparing) { return Decision(); }
        state_.verificationAvailable = false;
        state_.gameplay = Health::Offline;
        if (state_.control == Health::Lost && !state_.coordinated) {
            state_.room = RoomState::Closing;
            state_.match = MatchState::None;
            return Accept(Effect::FinishDegradedMatch);
        }
        state_.match = MatchState::PostMatch;
        return Accept();
    case EventKind::ControlLost:
        if (state_.room != RoomState::Joined && state_.room != RoomState::Opening) { return Decision(); }
        state_.control = Health::Lost;
        state_.verificationAvailable = false;
        state_.readyPending = false;
        state_.error = event.error.substr(0, MaxErrorLength);
        if (state_.coordinated) {
            state_.authorityWritable = false;
            // ObserveCoordination supplies the monotonic recovery clock.
            return Accept();
        }
        if (state_.match == MatchState::Playing && state_.gameplay == Health::Healthy) {
            state_.room = RoomState::Lost;
            return Accept();
        }
        state_.room = RoomState::Closing;
        return Accept(Effect::CloseSession);
    case EventKind::GameplayLost:
        if (state_.match != MatchState::Playing && state_.match != MatchState::Preparing) { return Decision(); }
        state_.gameplay = Health::Lost;
        state_.match = MatchState::Failed;
        state_.verificationAvailable = false;
        state_.room = RoomState::Closing;
        state_.error = event.error.substr(0, MaxErrorLength);
        return Accept(Effect::AbortMatch);
    case EventKind::MatchRecovered:
        // This event acknowledges local native/helper retirement. It cannot
        // depend on a remote quorum that may have disappeared with the match.
        if (state_.room != RoomState::Joined ||
            (state_.control != Health::Healthy && !state_.coordinated)) return Decision();
        state_.match = MatchState::PostMatch;
        state_.gameplay = Health::Offline;
        state_.verificationAvailable = false;
        state_.readyPending = false;
        state_.page = Page::Lobby;
        state_.error = event.error.substr(0, MaxErrorLength);
        return Accept();
    case EventKind::HelperLost:
        if (state_.room == RoomState::Idle) {
            // Offline remains usable, including after a failed helper startup.
            state_.error = event.error.substr(0, MaxErrorLength);
            return Accept();
        }
        state_.room = RoomState::Closing;
        state_.control = Health::Lost;
        state_.gameplay = Health::Lost;
        state_.verificationAvailable = false;
        state_.readyPending = false;
        state_.error = event.error.substr(0, MaxErrorLength);
        return Accept(Effect::CloseSession);
    }
    return Decision();
}



} } // namespace sf4e::netplay
