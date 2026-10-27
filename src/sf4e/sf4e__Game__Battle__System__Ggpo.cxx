// GGPO session lifecycle and spectator policy for sf4e::Game::Battle::System.
// Split from sf4e__Game__Battle__System.cxx; the callbacks are in
// sf4e__Game__Battle__System__GgpoCallbacks.cxx and pacing in
// sf4e__Game__Battle__System__GgpoPacing.cxx.
#include "sf4e__Game__Battle__System__Internal.hxx"
#include "sf4e__MementoGuards.hxx"
#include "../common/GgpoDisconnectTolerance.hxx"
#include "../common/SpectatorCatchUp.hxx"
#include "../common/BattleStep.hxx"
#include "../Dimps/Dimps.hxx"
#include <atomic>
#include <map>

sf4e::RollbackHud rollbackHud;

// Last disconnect_flags observed from ggpo_synchronize_input; logged on
// change for diagnostics only (no gameplay semantics attached).
static int s_lastDisconnectFlags = 0;

// An orphaned netplay battle normally leaves within a few updates. One
// error line after this many says the engine did not honour the exit.
static const uint32_t kOrphanOverdueFrames = 300;

// GGPO callback re-entrancy. ggpo_close_session deletes the backend, and the
// fork keeps calling the advance-frame callback from Sync::AdjustSimulation
// after the callback returns, so the session must never be closed from inside
// a callback. Aborts raised while a callback is on the stack are latched here
// and drained by DrainPendingAbort() once the top-level GGPO call returns.
sf4e::gate::AbortLatch s_abortLatch;

sf4e::SpectatorPolicy s_spectatorPolicy;
// GGPO alone would leave a dropped spectator waiting for a stream that never
// comes, until its own StreamSyncTimeoutMs or, once it runs, the match end, so
// P1 ends the dropped spectator's link too.
sf4e::SpectatorLinks s_spectatorLinks;
sf4e::SpectatorStreamWatch s_streamWatch;

std::size_t fSystem::SpectatorStreamCount() {
    // Only P1 (the fighter in players[0]) adds GGPO_PLAYERTYPE_SPECTATOR peers.
    if (!ggpo || sessionRole != SessionRole::HostFighter) return 0;
    return s_spectatorPolicy.Count();
}

void fSystem::PollSpectators() {
    const auto now = GetTickCount64();
    using StreamStep = sf4e::SpectatorStreamWatch::Step;
    const auto streamStep = ggpo ? s_streamWatch.Poll(now, MatchResultKnown()) : StreamStep::Wait;
    if (streamStep != StreamStep::Wait) {
        const bool lost = streamStep == StreamStep::EndLost;
        spdlog::log(lost ? spdlog::level::err : spdlog::level::info, "GGPO: spectator stream not synchronized within {} ms ({})",
            sf4e::SpectatorPolicy::StreamSyncTimeoutMs, lost ? "stream lost" : "the match result was already recorded");
        AbortGgpoMatch(lost ? sf4e::loc::T("runtime.spectator_stream_lost") : "");
        return;
    }
    // Drops noted since the last poll, the last spectator's among them.
    for (const auto slot : s_spectatorLinks.Take()) sf4e::NetplayFacade::EndRuntimeSpectatorStream(slot);
    if (!SpectatorStreamCount()) return;
    const auto drop = [&](int handle, const char* reason) {
        GGPONetworkStats stats = {};
        ggpo_get_network_stats(ggpo, handle, &stats);
        spdlog::info("GGPO: dropping spectator handle {} ({}; ping {} ms, backlog {} frames)",
            handle, reason, stats.network.ping, stats.network.send_queue_len);
        const auto result = ggpo_disconnect_player(ggpo, handle);
        if (!GGPO_SUCCEEDED(result) && result != GGPO_ERRORCODE_PLAYER_DISCONNECTED)
            spdlog::warn("GGPO: spectator handle {} could not be dropped: {}", handle, (int)result);
        s_spectatorLinks.Dropped(handle);
    };
    for (const auto handle : s_spectatorPolicy.SyncOverdue(now)) drop(handle, "not synchronized in time");
    if (!s_spectatorPolicy.SampleDue(now)) return;
    for (const auto handle : s_spectatorPolicy.Handles()) {
        GGPONetworkStats stats = {};
        if (!GGPO_SUCCEEDED(ggpo_get_network_stats(ggpo, handle, &stats))) continue;
        if (s_spectatorPolicy.Sample(handle, stats.network.send_queue_len, stats.network.ping)) drop(handle, "too far behind");
    }
}


void NoteDisconnectFlags(int flags) {
    if (flags != s_lastDisconnectFlags) {
        spdlog::info(
            "GGPO: disconnect_flags changed {:#x} -> {:#x}",
            s_lastDisconnectFlags,
            flags
        );
        s_lastDisconnectFlags = flags;
    }
}


// Disconnect countdown for the HUD: GGPO reports the timeout with the
// CONNECTION_INTERRUPTED event; the warning start time lives in simGate.
int s_disconnectTimeoutMs = 0;

int fSystem::DisconnectCountdownMs() {
    if (!ggpo || !simGate.connectionWarningActive || s_disconnectTimeoutMs <= 0) {
        return -1;
    }
    const uint32_t elapsed = GetTickCount() - simGate.connectionWarningStartedAtMs;
    return elapsed >= (uint32_t)s_disconnectTimeoutMs ? 0 : (int)(s_disconnectTimeoutMs - elapsed);
}

void fSystem::RetireGgpoSession(const char* diagnosticsLabel) {
    // A drop not yet taken belongs to this session; the next match's links must not be ended for it.
    s_spectatorLinks.Start();
    matchTelemetry.Reset();
    rollbackHud.Reset();
    if (ggpo) {
        if (diag::Enabled()) {
            diag::G().OnSessionEnded(diag::NowMs());
        }
        LogPacerSummary(diagnosticsLabel);
        if (s_abortLatch.InCallback()) {
            // Closing here would delete the backend under GGPO's own stack frame.
            // The pending-abort latch closes it from the outer tick instead.
            spdlog::error("GGPO: RetireGgpoSession({}) called from inside a GGPO callback; deferring", diagnosticsLabel);
            simGate.OnFatal();
            s_abortLatch.Request("");
            return;
        }
        ggpo_close_session(ggpo);
        ggpo = nullptr;
        s_abortLatch.Reset();
        sf4e::memento::DiscardPendingAbort();
        s_disconnectTimeoutMs = 0;
        // An Error says why the session ended (peer disconnected, stream
        // dropped, desync) and outlives it until a newer notice or the next
        // StartGGPO / StartSpectating.
        sf4e::NetplayFacade::ClearTransientMatchNotice();
        EmitRollbackDiagSummary(diagnosticsLabel);
        ResetPacing();
    }
    // With or without a session to close: a netplay battle that is still
    // alive here has lost its session and is orphaned. BattleUpdate drives
    // it out (LeaveOrphanedNetplayBattle); nothing here touches the gate,
    // so an offline battle that follows is never left frozen.
    simGate.OnSessionClosed();
    // No session, so no role and nothing left to drain. Every reader of the
    // role is gated on a live session or runs inside a callback.
    sessionRole = SessionRole::None;
    drainingSpectators = false;
}

// The engine update for a netplay battle whose session is gone. Nobody may
// play it meanwhile: both slots read neutral playback input, through the
// same path GGPO's inputs take, and the exit is re-asserted every update
// because the native flow can overwrite it while a round is starting. The
// ready state is only ever raised, so RS_HALTED is never pulled back.
void LeaveOrphanedNetplayBattle(rSystem* system) {
    int& readyState = *rSystem::GetReadyState(system);
    if (fSystem::simGate.OnOrphanFrame()) {
        // Once the result is known this is the ordinary way a spectator
        // leaves the tail of a match whose stream has ended.
        spdlog::log(MatchResultKnown() ? spdlog::level::info : spdlog::level::warn,
            "Netplay battle lost its session; leaving it ready={} flow={}",
            readyState, *rSystem::staticVars.CurrentBattleFlow);
    }
    if (readyState < rSystem::RS_ISLEAVING) {
        readyState = rSystem::RS_ISLEAVING;
    }
    if (fSystem::simGate.OrphanOverdue(kOrphanOverdueFrames)) {
        spdlog::error("Netplay battle still alive {} updates after losing its session ready={} flow={}",
            kOrphanOverdueFrames, readyState, *rSystem::staticVars.CurrentBattleFlow);
    }
    const fPadSystem::Inputs neutral[2] = { {0, 0}, {0, 0} };
    fSystem::StepEngine(system, neutral, true);
}

void fSystem::AbortGgpoMatch(const char* reason, sf4e::NoticeSeverity severity, AbortCause cause) {
    if (s_abortLatch.Request(reason)) {
        // Inside a GGPO callback: mark the session fatal so the remaining
        // callbacks of this burst do no engine work, remember the reason, and
        // let the outer tick close the session once GGPO has unwound.
        simGate.OnFatal();
        spdlog::error("GGPO match abort deferred from callback (depth {}): {}", s_abortLatch.depth, reason ? reason : "");
        return;
    }
    const bool explained = reason && reason[0];
    if (explained) {
        spdlog::log(severity == sf4e::NoticeSeverity::Error ? spdlog::level::err : spdlog::level::info,
            "GGPO match abort: {}", reason);
    }
    LogSaveSlotOccupancy("abort_entry");
    // A spectator's session retired before the room knows the match's end is its
    // own stream failure, unless the player left it. The runtime learns why when
    // the battle closes, and leaves the game without its lock-in instead of
    // ending the view silently.
    if (cause == AbortCause::Failure && ggpo && !LocalFighter() && !MatchResultKnown()) {
        sf4e::NetplayFacade::NotifyRuntimeSpectatorStreamFailed();
    }
    simGate.OnFatal();
    RetireGgpoSession("abort");
    sf4e::NetplayFacade::ClearBattleState();
    // Retiring the session clears its transient notices, so the reason is
    // published after it, and it replaces any Error the session kept.
    if (explained) {
        sf4e::NetplayFacade::PushAlert(reason, severity);
    }
}

void EndSpectatorStream(int ggpoResult) {
    int backlog = -1;
    if (fSystem::ggpo) ggpo_get_spectator_backlog(fSystem::ggpo, &backlog);
    // The room already recorded this match's end, so the spectator only
    // loses the tail of a finished match. Nothing needs the player's attention.
    if (MatchResultKnown()) {
        spdlog::info("GGPO: spectator stream ended after the match result was recorded (synchronize_input {}, backlog {})",
            ggpoResult, backlog);
        fSystem::AbortGgpoMatch("");
        return;
    }
    spdlog::error("GGPO: spectator stream lost: synchronize_input returned {} (backlog {})", ggpoResult, backlog);
    fSystem::AbortGgpoMatch(sf4e::loc::T("runtime.spectator_stream_lost"));
}

bool fSystem::DrainPendingAbort() {
    char reason[256];
    if (!s_abortLatch.Take(reason, sizeof(reason))) {
        return false;
    }
    AbortGgpoMatch(reason);
    return true;
}

static sf4e::battlestep::MatchStep s_matchStep;

void fSystem::FixBattleStepForMatch() {
    if (!Dimps::App::battleStepSetting || !Dimps::App::SetBattleStepSetting) return;
    const float setting = *Dimps::App::battleStepSetting;
    if (!s_matchStep.Begin(setting)) return;
    Dimps::App::SetBattleStepSetting(sf4e::battlestep::OneFrame);
    spdlog::info("Battle step: one frame at a time for this match; the game's setting was {} (0: FrameRate=SMOOTH or no FrameRate line)", setting);
}

void fSystem::RestoreBattleStep() {
    if (!s_matchStep.End() || !Dimps::App::SetBattleStepSetting) return;
    Dimps::App::SetBattleStepSetting(s_matchStep.Saved());
    spdlog::info("Battle step: the game's setting {} is back", s_matchStep.Saved());
}

// The callback set both session kinds use (NETPLAY_INVARIANTS.md).
static GGPOSessionCallbacks SessionCallbacks() {
    GGPOSessionCallbacks cb = { 0 };
    cb.begin_game = fSystem::ggpo_begin_game_callback;
    cb.advance_frame = fSystem::ggpo_advance_frame_callback;
    cb.load_game_state = fSystem::ggpo_load_game_state_callback;
    cb.save_game_state = fSystem::ggpo_save_game_state_callback;
    cb.free_buffer = fSystem::ggpo_free_buffer;
    cb.on_event = fSystem::ggpo_on_event_callback;
    cb.log_game_state = fSystem::ggpo_log_game_state;
    return cb;
}

// The one session start sequence, for a fighter's session and a spectator's,
// before GGPO is asked for either. The order is: a leftover session retired
// (which resets the telemetry, the HUD and the role itself), then everything
// the new session starts clean with.
void fSystem::BeginSession(const char* label, bool spectator) {
    rollbackHud.Reset();
    FixBattleStepForMatch();
    diag::InitFromEnvironment();
    sf4e::NetplayFacade::CancelDeferredGgpoClose();
    if (ggpo) {
        spdlog::warn("{}: closing leftover GGPO session before {}", label, spectator ? "restart" : "rematch/restart");
        RetireGgpoSession(spectator ? "leftover_before_spectating" : "leftover_before_start");
    }
    matchTelemetry.Reset(spectator);
    diag::G().ResetForMatch(diag::NowMs());
    ResetNativeResultMatch();
    // A new session starts clean: the last match's abort reason is moot.
    sf4e::NetplayFacade::ClearMatchNotice();
    // No fighter handles yet (and none at all on a spectator client); stale
    // ones from an earlier match must not classify this session's events.
    for (auto& player : players) { player = {}; player.handle = GGPO_INVALID_HANDLE; }
    // The savestate pool must start empty. A slot still holding records here
    // is leaked from the previous match, most often via the deferred-close
    // path, which retires the session from NetplayFacade::TickFrame and so
    // never runs CloseBattle's free loop. Reclaim without engine calls: the
    // previous battle's objects are gone, so ClearKey through those pointers
    // would fault. Reusing a dirty slot is what corrupts the next match.
    LogSaveSlotOccupancy(spectator ? "start_spectating_entry" : "start_ggpo_entry");
    LogSaveStateFreePolicy();
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        SaveState::Reclaim(&saveStates[i], spectator ? "start_spectating" : "start_ggpo", i);
    }
    simGate.OnSessionStarted();
    s_streamWatch.Start(GetTickCount64(), spectator);
    ResetPacerForSession();
    s_lastDisconnectFlags = 0;
    s_disconnectTimeoutMs = 0;
    s_abortLatch.Reset();
    sf4e::memento::DiscardPendingAbort();
    localPlayerHandle = GGPO_INVALID_HANDLE;
    // A fighter's role is known once its local handle is (StartGGPO).
    sessionRole = spectator ? SessionRole::Spectator : SessionRole::None;
    ggpoTimeline.Reset();
    // A claimed netplay battle leaves straight back to the menu, past the
    // native start and results flow, whether or not its session then starts:
    // one that does not is orphaned and leaves on its own (F-016).
    nNextBattleStartFlowTarget = BF__MATCH_START;
    fVsBattle::bTerminateOnNextLeftBattle = true;
}

bool fSystem::StartGGPO(GGPOPlayer* inPlayers, int numPlayers, int port, int frameDelay, std::uint64_t spectatorSyncDeadlineMs,
    const char*& failure) {
    if (!inPlayers || numPlayers < 2 || numPlayers > static_cast<int>(sf4e::room::MaxMatchParticipants)) {
        failure = sf4e::loc::T("runtime.invalid_roster");
        return false;
    }
    BeginSession("StartGGPO", false);

    GGPOSessionCallbacks cb = SessionCallbacks();
    spdlog::info(
        "GGPO: starting session localPort={} players={} frameDelay={}",
        port,
        numPlayers,
        frameDelay
    );
    GGPOErrorCode result = ggpo_start_session(
        &ggpo,
        &cb,
        "sf4e",
        2,
        sizeof(fPadSystem::Inputs),
        port
    );
    if (result != GGPO_OK) {
        spdlog::error("GGPO session could not start: {}", (int)result);
        ggpo = nullptr;
        failure = sf4e::loc::T("runtime.ggpo_start_failed");
        return false;
    }
    spdlog::info("GGPO: session started localPort={}", port);
    spdlog::info("GGPO: fp {}", sf4e::statehash::FpEnvironment());
    sf4e::crash::NoteMatchBoundary("start_ggpo");
    // Fighter links only (NETPLAY_INVARIANTS.md): the spectator backend rejects it.
    sf4e::GgpoDisconnectTolerance::Apply(ggpo);

    int localPlayerIdx = -1;
    for (int i = 0; i < 2; i++) {
        players[i].type = inPlayers[i].type;
        result = ggpo_add_player(ggpo, inPlayers + i, &players[i].handle);
        if (!GGPO_SUCCEEDED(result)) {
            spdlog::error("GGPO session could not add player: {}", (int)result);
            failure = sf4e::loc::T("runtime.ggpo_add_players_failed");
            return false;
        }

        if (players[i].type == GGPO_PLAYERTYPE_LOCAL) {
            const auto delayResult = ggpo_set_frame_delay(ggpo, players[i].handle, frameDelay);
            if (!GGPO_SUCCEEDED(delayResult)) spdlog::warn("GGPO: set_frame_delay({}) returned {}", frameDelay, (int)delayResult);
            matchTelemetry.AppliedDelay(frameDelay, GGPO_SUCCEEDED(delayResult));
            localPlayerHandle = players[i].handle;
            sessionRole = i == 0 ? SessionRole::HostFighter : SessionRole::Fighter;
            localPlayerIdx = i;
        }
    }
    std::vector<int> spectatorHandles;
    std::map<int, std::size_t> spectatorSlots;
    if (localPlayerIdx == 0) {
        for (int i = 2; i < numPlayers; i++) {
            players[i].type = inPlayers[i].type;
            result = ggpo_add_player(ggpo, inPlayers + i, &players[i].handle);
            if (!GGPO_SUCCEEDED(result)) {
                spdlog::error("GGPO session could not add spectator: {}", (int)result);
                if (sf4e::NetplayFacade::IsDevOverlayEnabled()) {
                    MessageBoxA(NULL, "GGPO could not add spectator", NULL, MB_OK);
                }
                continue;
            }
            if (players[i].handle == GGPO_INVALID_HANDLE) continue;
            spectatorHandles.push_back(players[i].handle);
            // player_num is the match slot plus one (StartRuntimeGgpo leaves out spectators without a port).
            spectatorSlots[players[i].handle] = static_cast<std::size_t>(inPlayers[i].player_num - 1);
        }
    }
    s_spectatorPolicy.Start(GetTickCount64(), spectatorHandles, spectatorSyncDeadlineMs);
    s_spectatorLinks.Start(std::move(spectatorSlots));
    return true;
}

bool fSystem::StartSpectating(unsigned short localport, int num_players, char* host_ip, unsigned short host_port,
    const char*& failure) {
    BeginSession("StartSpectating", true);
    GGPOSessionCallbacks cb = SessionCallbacks();

    GGPOErrorCode result = ggpo_start_spectating(
        &ggpo,
        &cb,
        "sf4e",
        num_players,
        sizeof(fPadSystem::Inputs),
        localport,
        host_ip,
        host_port
    );
    if (result != GGPO_OK) {
        spdlog::error("GGPO session could not start: {}", (int)result);
        if (sf4e::NetplayFacade::IsDevOverlayEnabled()) {
            MessageBoxA(NULL, "GGPO could not start, check logs", NULL, MB_OK);
        }
    }
    sf4e::crash::NoteMatchBoundary("start_spectating");
    // No disconnect tolerance: GGPO's spectator backend has no silence timeout, so
    // this link ends through the match session and the teardown timers.
    const bool started = result == GGPO_OK && ggpo != nullptr;
    if (!started) failure = sf4e::loc::T("runtime.ggpo_start_failed");
    return started;
}

