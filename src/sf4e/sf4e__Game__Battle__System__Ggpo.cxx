// GGPO session lifecycle, callbacks, spectator policy and pacing for
// sf4e::Game::Battle::System. Split from sf4e__Game__Battle__System.cxx.
#include "sf4e__Game__Battle__System__Internal.hxx"

static sf4e::RollbackHud rollbackHud;

// Always-on netplay time series: one log line per 15 s window and one at the
// end of a session, so tester logs show rift, ping, rollbacks and stalls
// without the diagnostics switch. Pacer totals are logged as window deltas.
struct NetplayWindow {
    uint64_t startMs = 0; // 0 until the first session starts
    uint32_t rollbacks = 0, resimFrames = 0, depth = 0, maxDepth = 0, stallTicks = 0;
    int pingMin = -1, pingMax = -1, localBehind = 0, remoteBehind = 0;
    double maxAbsRift = 0.0;
    uint32_t recsAtStart = 0;
    double slowedAtStart = 0.0, spedUpAtStart = 0.0;

    void Start(uint64_t now) {
        *this = {};
        startMs = now;
        recsAtStart = fSystem::pacer.recommendationsReceived;
        slowedAtStart = fSystem::pacer.msSlowedTotal;
        spedUpAtStart = fSystem::pacer.msSpedUpTotal;
    }

    void Sample(const GGPONetworkStats& stats) {
        const double ema = fSystem::pacer.riftFramesEma;
        maxAbsRift = (std::max)(maxAbsRift, ema < 0.0 ? -ema : ema);
        localBehind = stats.timesync.local_frames_behind;
        remoteBehind = stats.timesync.remote_frames_behind;
        // GGPO reports 0 until the first round-trip reply arrives.
        const int ping = stats.network.ping;
        if (ping <= 0) return;
        pingMin = pingMin < 0 ? ping : (std::min)(pingMin, ping);
        pingMax = (std::max)(pingMax, ping);
    }

    // Logs the window so far and starts the next one.
    void Flush(uint64_t now) {
        const sf4e::pacing::PacingController& p = fSystem::pacer;
        if (startMs != 0 && now > startMs) {
            spdlog::info(
                "Netplay [{:.0f}s]: ping={}..{} riftFrames={:.2f} maxAbsRift={:.2f} behind={}/{} rollbacks={} "
                "resimFrames={} maxDepth={} stallTicks={} slowedMs={:.1f} spedUpMs={:.1f} timesyncEvents={}",
                (now - startMs) / 1000.0, pingMin, pingMax, p.riftFramesEma, maxAbsRift, localBehind, remoteBehind,
                rollbacks, resimFrames, maxDepth, stallTicks, p.msSlowedTotal - slowedAtStart,
                p.msSpedUpTotal - spedUpAtStart, p.recommendationsReceived - recsAtStart);
        }
        Start(now);
    }
};
static NetplayWindow s_window;
static constexpr uint64_t kNetplayWindowMs = 15000;

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
static sf4e::gate::AbortLatch s_abortLatch;

struct GgpoCallbackScope : sf4e::gate::AbortLatch::Scope {
    GgpoCallbackScope() : sf4e::gate::AbortLatch::Scope(s_abortLatch) {}
};

static sf4e::SpectatorPolicy s_spectatorPolicy;

std::size_t fSystem::SpectatorStreamCount() {
    // players[0] is the local handle only on P1, which is the only client that
    // adds GGPO_PLAYERTYPE_SPECTATOR peers.
    if (!ggpo || localPlayerHandle == GGPO_INVALID_HANDLE || players[0].handle != localPlayerHandle) return 0;
    return s_spectatorPolicy.Count();
}

void fSystem::PollSpectators() {
    if (!SpectatorStreamCount()) return;
    const auto now = GetTickCount64();
    const auto drop = [&](int handle, const char* reason) {
        spdlog::info("GGPO: dropping spectator handle {} ({})", handle, reason);
        const auto result = ggpo_disconnect_player(ggpo, handle);
        if (!GGPO_SUCCEEDED(result) && result != GGPO_ERRORCODE_PLAYER_DISCONNECTED)
            spdlog::warn("GGPO: spectator handle {} could not be dropped: {}", handle, (int)result);
    };
    for (const auto handle : s_spectatorPolicy.SyncOverdue(now)) drop(handle, "not synchronized in time");
    if (!s_spectatorPolicy.SampleDue(now)) return;
    for (const auto handle : s_spectatorPolicy.Handles()) {
        GGPONetworkStats stats = {};
        if (!GGPO_SUCCEEDED(ggpo_get_network_stats(ggpo, handle, &stats))) continue;
        if (s_spectatorPolicy.Sample(handle, stats.network.send_queue_len)) drop(handle, "too far behind");
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
static int s_disconnectTimeoutMs = 0;

int fSystem::DisconnectCountdownMs() {
    if (!ggpo || !simGate.connectionWarningActive || s_disconnectTimeoutMs <= 0) {
        return -1;
    }
    const uint32_t elapsed = GetTickCount() - simGate.connectionWarningStartedAtMs;
    return elapsed >= (uint32_t)s_disconnectTimeoutMs ? 0 : (int)(s_disconnectTimeoutMs - elapsed);
}

// Applies development overrides for the pacing caps and resets the
// controller for a new session. Called from StartGGPO/StartSpectating.
static void ResetPacerForSession() {
    fSystem::pacer.InitDefaults();
    sf4e::Platform::D3D::CancelFrameShift();
    const char* enabledEnv = getenv("SF4E_GGPO_DISTRIBUTED_TIMESYNC");
    fSystem::pacer.enabled = !(enabledEnv && enabledEnv[0] == '0');
    const char* stepEnv = getenv("SF4E_PACING_MAX_STEP_MS");
    if (stepEnv && stepEnv[0]) {
        double v = atof(stepEnv);
        if (v > 0.0 && v <= 20.0) {
            fSystem::pacer.maxStepMs = v;
        }
    }
    const char* framesEnv = getenv("SF4E_PACING_MAX_FRAMES");
    if (framesEnv && framesEnv[0]) {
        double v = atof(framesEnv);
        if (v > 0.0 && v <= 30.0) {
            fSystem::pacer.maxRecommendationFrames = v;
        }
    }
    // Both experiments are on unless set to 0. GGPO reads the repair switch
    // itself with the EnvFlag rule. The line lets a log confirm which side ran
    // what.
    fSystem::pacer.continuous = sf4e::EnvFlag("SF4E_CONTINUOUS_TIMESYNC", true);
    spdlog::info("Netplay experiments: continuousTimesync={} inputRepair={} build={}",
        fSystem::pacer.continuous, sf4e::EnvFlag("SF4E_GGPO_INPUT_REPAIR", true), sf4e::sidecarHash);
    s_window.Start(GetTickCount64());
}

void fSystem::ResetPacing() {
    pacer.Reset();
    sf4e::Platform::D3D::CancelFrameShift();
}

void LogPacerSummary(const char* label) {
    s_window.Flush(GetTickCount64());
    const sf4e::pacing::PacingController& p = fSystem::pacer;
    if (p.recommendationsReceived == 0 && p.msSlowedTotal == 0.0 && p.msSpedUpTotal == 0.0 && p.riftSamples == 0) {
        return;
    }
    spdlog::info(
        "Pacing [{}]: enabled={} recs={} framesRec={} acceptedMs={:.1f} "
        "replacedMs={:.1f} disabledDiscardMs={:.1f} resetDiscardMs={:.1f} "
        "slowedMs={:.1f} spedUpMs={:.1f} maxShiftMs={:.2f} "
        "maxOutstandingMs={:.1f} outstandingMs={:.1f} continuous={} riftSamples={} "
        "riftAcceptedMs={:.1f} riftFrames={:.2f} maxAbsRiftFrames={:.2f}",
        label,
        p.enabled,
        p.recommendationsReceived,
        p.framesRecommendedTotal,
        p.msAcceptedTotal,
        p.msReplacedTotal,
        p.msDiscardedDisabled,
        p.msDiscardedOnReset,
        p.msSlowedTotal,
        p.msSpedUpTotal,
        p.maxSingleShiftMs,
        p.maxOutstandingMs,
        p.outstandingMs,
        p.continuous,
        p.riftSamples,
        p.msRiftAcceptedTotal,
        p.riftFramesEma,
        p.maxAbsRiftFrames
    );
}

void fSystem::ApplyGgpoDisconnectSettings(GGPOSession* session) {
    if (!session) {
        return;
    }

    const sf4e::NetplayConfig& cfg = sf4e::NetplayFacade::GetConfig();
    uint16_t timeoutMs = 3000;
    uint16_t notifyMs = 1500;
    if (cfg.version >= 8 && cfg.ggpoDisconnectTimeoutMs > 0) {
        timeoutMs = cfg.ggpoDisconnectTimeoutMs;
        notifyMs = cfg.ggpoDisconnectNotifyMs > 0
            ? cfg.ggpoDisconnectNotifyMs
            : (uint16_t)(timeoutMs / 2);
    }
    else {
        timeoutMs = (uint16_t)(1000 + cfg.inputDelay * 500);
        if (timeoutMs < 3000) {
            timeoutMs = 3000;
        }
        notifyMs = (uint16_t)(timeoutMs / 2);
    }

    ggpo_set_disconnect_timeout(session, timeoutMs);
    ggpo_set_disconnect_notify_start(session, notifyMs);
}

void fSystem::RetireGgpoSession(const char* diagnosticsLabel) {
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
        s_disconnectTimeoutMs = 0;
        sf4e::NetplayFacade::ClearMatchNotice();
        EmitRollbackDiagSummary(diagnosticsLabel);
        ResetPacing();
    }
    // With or without a session to close: a netplay battle that is still
    // alive here has lost its session and is orphaned. BattleUpdate drives
    // it out (LeaveOrphanedNetplayBattle); nothing here touches the gate,
    // so an offline battle that follows is never left frozen.
    simGate.OnSessionClosed();
}

// The engine update for a netplay battle whose session is gone. Nobody may
// play it meanwhile: both slots read neutral playback input, through the
// same path GGPO's inputs take, and the exit is re-asserted every update
// because the native flow can overwrite it while a round is starting. The
// ready state is only ever raised, so RS_HALTED is never pulled back.
void LeaveOrphanedNetplayBattle(rSystem* system) {
    int& readyState = *rSystem::GetReadyState(system);
    if (fSystem::simGate.OnOrphanFrame()) {
        spdlog::warn("Netplay battle lost its session; leaving it ready={} flow={}",
            readyState, *rSystem::staticVars.CurrentBattleFlow);
    }
    if (readyState < rSystem::RS_ISLEAVING) {
        readyState = rSystem::RS_ISLEAVING;
    }
    if (fSystem::simGate.OrphanOverdue(kOrphanOverdueFrames)) {
        spdlog::error("Netplay battle still alive {} updates after losing its session ready={} flow={}",
            kOrphanOverdueFrames, readyState, *rSystem::staticVars.CurrentBattleFlow);
    }
    PlaybackFrameScopeGuard _playbackGuard;
    fPadSystem::playbackFrame = 0;
    fPadSystem::playbackData[0][0] = { 0, 0 };
    fPadSystem::playbackData[0][1] = { 0, 0 };
    if (fSoundPlayerManager::bUsePureSounds) {
        fSoundPlayerManager::SyncState();
    }
    (system->*rSystem::publicMethods.BattleUpdate)();
}

void fSystem::AbortGgpoMatch(const char* reason) {
    if (s_abortLatch.Request(reason)) {
        // Inside a GGPO callback: mark the session fatal so the remaining
        // callbacks of this burst do no engine work, remember the reason, and
        // let the outer tick close the session once GGPO has unwound.
        simGate.OnFatal();
        spdlog::error("GGPO match abort deferred from callback (depth {}): {}", s_abortLatch.depth, reason ? reason : "");
        return;
    }
    if (reason && reason[0]) {
        spdlog::error("GGPO match abort: {}", reason);
        sf4e::NetplayFacade::PushAlert(reason, sf4e::NoticeSeverity::Error);
    }
    LogSaveSlotOccupancy("abort_entry");
    simGate.OnFatal();
    RetireGgpoSession("abort");
    sf4e::NetplayFacade::ClearBattleState();
}

bool fSystem::DrainPendingAbort() {
    char reason[256];
    if (!s_abortLatch.Take(reason, sizeof(reason))) {
        return false;
    }
    AbortGgpoMatch(reason);
    return true;
}

void fSystem::StartGGPO(GGPOPlayer* inPlayers, int numPlayers, int port, int frameDelay, DWORD rngSeed) {
    matchTelemetry.Reset();
    rollbackHud.Reset();
    if (!inPlayers || numPlayers < 2 || numPlayers > static_cast<int>(sf4e::room::MaxMatchParticipants)) {
        sf4e::NetplayFacade::PushAlert(sf4e::loc::T("runtime.invalid_roster"));
        return;
    }
    diag::InitFromEnvironment();
    sf4e::NetplayFacade::CancelDeferredGgpoClose();
    if (ggpo) {
        spdlog::warn("StartGGPO: closing leftover GGPO session before rematch/restart");
        RetireGgpoSession("leftover_before_start");
    }
    diag::G().ResetForMatch(diag::NowMs());
    ResetNativeResultMatch();
    for (auto& player : players) { player = {}; player.handle = GGPO_INVALID_HANDLE; }
    // The savestate pool must start empty. A slot still holding records here
    // is leaked from the previous match — most often via the deferred-close
    // path, which retires the session from NetplayFacade::TickFrame and so
    // never runs CloseBattle's free loop. Reclaim without engine calls: the
    // previous battle's objects are gone, so ClearKey through those pointers
    // would fault. Reusing a dirty slot is what corrupts the next match.
    LogSaveSlotOccupancy("start_ggpo_entry");
    LogSaveStateFreePolicy();
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        SaveState::Reclaim(&saveStates[i], "start_ggpo", i);
    }
    simGate.OnSessionStarted();
    ResetPacerForSession();
    s_lastDisconnectFlags = 0;
    s_disconnectTimeoutMs = 0;
    s_abortLatch.Reset();
    localPlayerHandle = GGPO_INVALID_HANDLE;
    lastGgpoSaveFrame = -1;

    GGPOSessionCallbacks cb = { 0 };
    cb.begin_game = ggpo_begin_game_callback;
    cb.advance_frame = ggpo_advance_frame_callback;
    cb.load_game_state = ggpo_load_game_state_callback;
    cb.save_game_state = ggpo_save_game_state_callback;
    cb.free_buffer = ggpo_free_buffer;
    cb.on_event = ggpo_on_event_callback;
    cb.log_game_state = ggpo_log_game_state;

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
        AbortGgpoMatch(sf4e::loc::T("runtime.ggpo_start_failed"));
        return;
    }
    spdlog::info("GGPO: session started localPort={}", port);
    spdlog::info("GGPO: fp {}", sf4e::statehash::FpEnvironment());
    ApplyGgpoDisconnectSettings(ggpo);

    int localPlayerIdx = -1;
    for (int i = 0; i < 2; i++) {
        players[i].type = inPlayers[i].type;
        result = ggpo_add_player(ggpo, inPlayers + i, &players[i].handle);
        if (!GGPO_SUCCEEDED(result)) {
            spdlog::error("GGPO session could not add player: {}", (int)result);
            AbortGgpoMatch(sf4e::loc::T("runtime.ggpo_add_players_failed"));
            return;
        }

        if (players[i].type == GGPO_PLAYERTYPE_LOCAL) {
            const auto delayResult = ggpo_set_frame_delay(ggpo, players[i].handle, frameDelay);
            matchTelemetry.AppliedDelay(frameDelay, GGPO_SUCCEEDED(delayResult));
            localPlayerHandle = players[i].handle;
            localPlayerIdx = i;
        }
    }
    std::vector<int> spectatorHandles;
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
            if (players[i].handle != GGPO_INVALID_HANDLE) spectatorHandles.push_back(players[i].handle);
        }
    }
    s_spectatorPolicy.Start(GetTickCount64(), spectatorHandles);

    nNextBattleStartFlowTarget = BF__MATCH_START;
    fVsBattle::bTerminateOnNextLeftBattle = true;
    fVsBattle::bOverrideNextRandomSeed = true;
    fVsBattle::nextMatchRandomSeed = rngSeed;
}

void fSystem::StartSpectating(unsigned short localport, int num_players, char* host_ip, unsigned short host_port, DWORD rngSeed) {
    rollbackHud.Reset();
    diag::InitFromEnvironment();
    sf4e::NetplayFacade::CancelDeferredGgpoClose();
    if (ggpo) {
        spdlog::warn("StartSpectating: closing leftover GGPO session before restart");
        RetireGgpoSession("leftover_before_spectating");
    }
    matchTelemetry.Reset(true);
    diag::G().ResetForMatch(diag::NowMs());
    ResetNativeResultMatch();
    // Same rationale as StartGGPO: the pool must start empty.
    LogSaveSlotOccupancy("start_spectating_entry");
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        SaveState::Reclaim(&saveStates[i], "start_spectating", i);
    }
    simGate.OnSessionStarted();
    ResetPacerForSession();
    s_lastDisconnectFlags = 0;
    s_disconnectTimeoutMs = 0;
    s_abortLatch.Reset();
    // No fighter handles on a spectator client; stale ones from an earlier
    // match must not classify the host stream's events.
    for (auto& player : players) { player = {}; player.handle = GGPO_INVALID_HANDLE; }
    localPlayerHandle = GGPO_INVALID_HANDLE;
    lastGgpoSaveFrame = -1;
    GGPOSessionCallbacks cb = { 0 };
    cb.begin_game = ggpo_begin_game_callback;
    cb.advance_frame = ggpo_advance_frame_callback;
    cb.load_game_state = ggpo_load_game_state_callback;
    cb.save_game_state = ggpo_save_game_state_callback;
    cb.free_buffer = ggpo_free_buffer;
    cb.on_event = ggpo_on_event_callback;
    cb.log_game_state = ggpo_log_game_state;

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
    ApplyGgpoDisconnectSettings(ggpo);

    nNextBattleStartFlowTarget = BF__MATCH_START;
    fVsBattle::bTerminateOnNextLeftBattle = true;
    fVsBattle::bOverrideNextRandomSeed = true;
    fVsBattle::nextMatchRandomSeed = rngSeed;
}

bool fSystem::ggpo_begin_game_callback(const char*)
{
    return true;
}

unsigned fSystem::RecentRollbackFrames() { return rollbackHud.Recent(GetTickCount64()); }
sf4e::MatchTelemetry fSystem::matchTelemetry;
void fSystem::PollMatchTelemetry() {
    if (!ggpo) return;
    const auto now = GetTickCount64();
    // A spectator has no local fighter handle; do not label its host link as fighter RTT.
    matchTelemetry.spectator = localPlayerHandle == GGPO_INVALID_HANDLE;
    if (!matchTelemetry.PollDue(now)) return;
    GGPONetworkStats stats{};
    matchTelemetry.Sample(now, !simGate.connectionWarningActive && GetRemoteNetworkStats(stats) ? stats.network.ping : -1);
}

bool fSystem::GetRemoteNetworkStats(GGPONetworkStats& stats) {
    if (!ggpo) return false;
    for (int i = 0; i < MAX_SF4E_PROTOCOL_USERS; i++) {
        if (players[i].type != GGPO_PLAYERTYPE_REMOTE) continue;
        const GGPOErrorCode result = ggpo_get_network_stats(ggpo, players[i].handle, &stats);
        if (diag::Enabled()) diag::G().RecordGgpoResult(diag::CALL_GET_NETWORK_STATS, (int)result);
        return GGPO_SUCCEEDED(result);
    }
    return false;
}

// Rift pacing, once per outer tick and never inside a GGPO callback. Collects
// what the frame limiter applied, samples the rift (in every mode, so baseline
// logs report it too), and requests the next frame's shift from the limiter
// (fD3D::LimitFrame). Deterministic simulation is never skipped or doubled;
// only frame length changes. A stalled tick already repays time and its
// advantage pair is unreliable, so it holds the estimate and does not shift.
fSystem::PacingTick fSystem::StepPacing() {
    PacingTick tick{ 0.0, 0.0 };
    if (!ggpo) return tick;
    tick.appliedMs = sf4e::Platform::D3D::TakeAppliedShift();
    pacer.OnShiftApplied(tick.appliedMs);
    if (tick.appliedMs != 0.0 && diag::Enabled()) {
        diag::G().RecordOp(diag::OP_PACING_WAIT, tick.appliedMs < 0.0 ? -tick.appliedMs : tick.appliedMs);
    }
    const uint64_t now = GetTickCount64();
    if (now - s_window.startMs >= kNetplayWindowMs) s_window.Flush(now);
    if (simGate.predictionStalled) {
        ++s_window.stallTicks;
        pacer.OnPredictionStall();
    }
    else if (MayAdvanceDeterministicFrame()) {
        GGPONetworkStats stats{};
        if (GetRemoteNetworkStats(stats)) {
            pacer.OnRiftSample(stats.timesync.local_frames_behind, stats.timesync.remote_frames_behind);
            s_window.Sample(stats);
        }
        tick.requestedMs = pacer.NextShiftMs();
    }
    sf4e::Platform::D3D::RequestFrameShift(tick.requestedMs);
    return tick;
}

bool fSystem::ggpo_advance_frame_callback(int)
{
    GgpoCallbackScope _callbackScope;
    diag::ScopedTimer _cbTimer(diag::OP_ROLLBACK_CALLBACK);
    if (diag::Enabled()) {
        diag::G().OnRollbackCallback(diag::NowMs());
    }

    // Once the session is fatal (an earlier callback in this burst aborted),
    // GGPO still calls back for the remaining frames. Do no engine work.
    if (!ggpo || simGate.fatalError) {
        return true;
    }
    // The battle is closed and its slots freed; a session kept open for
    // spectators may still resimulate on a late input. Keep GGPO's frame
    // count moving without touching the engine.
    if (sf4e::NetplayFacade::DrainingSpectators()) {
        ggpo_advance_frame(ggpo);
        return true;
    }

    fPadSystem::Inputs inputs[2] = { {0, 0}, {0, 0} };
    int disconnect_flags = 0;

    // Make sure we fetch new inputs from GGPO and use those to update
    // the game state instead of reading from the selected input device.
    GGPOErrorCode result = ggpo_synchronize_input(ggpo, (void*)inputs, sizeof(fPadSystem::Inputs) * 2, &disconnect_flags);
    if (diag::Enabled()) {
        diag::G().RecordGgpoResult(diag::CALL_SYNC_INPUT, (int)result);
    }
    if (!GGPO_SUCCEEDED(result)) {
        AbortGgpoMatch(sf4e::loc::T("runtime.netplay_sync_failed"));
        return true;
    }
    NoteDisconnectFlags(disconnect_flags);

    // Restored on every exit; rollback resimulation must never leak
    // playback mode into subsequent engine work.
    PlaybackFrameScopeGuard _playbackGuard;
    fPadSystem::playbackFrame = 0;
    fPadSystem::playbackData[0][0] = inputs[0];
    fPadSystem::playbackData[0][1] = inputs[1];

    // Actually update.
    // It's important that this calls the _original_, undetoured method-
    // if it called fSystem::BattleUpdate, it'd be restricted to the same
    // update-halting that the detoured method is.
    rSystem* system = rSystem::staticMethods.GetSingleton();
    {
        diag::ScopedTimer _t(diag::OP_ENGINE_BATTLE_UPDATE);
        (system->*rSystem::publicMethods.BattleUpdate)();
    }

    result = ggpo_advance_frame(ggpo);
    if (diag::Enabled()) {
        diag::G().RecordGgpoResult(diag::CALL_ADVANCE_FRAME, (int)result);
    }
    if (!GGPO_SUCCEEDED(result)) {
        AbortGgpoMatch(sf4e::loc::T("runtime.netplay_sync_failed"));
    }
    else {
        rollbackHud.Replayed(GetTickCount64());
        ++s_window.resimFrames;
        s_window.maxDepth = (std::max)(s_window.maxDepth, ++s_window.depth);
        CaptureSnapshot(system);
        CaptureHashCheckpoint(system);
    }

    return true;
}

bool fSystem::ggpo_load_game_state_callback(unsigned char* buffer, int len)
{
    GgpoCallbackScope _callbackScope;
    if (simGate.fatalError || sf4e::NetplayFacade::DrainingSpectators()) {
        return true;
    }
    rollbackHud.Begin(GetTickCount64());
    ++s_window.rollbacks;
    s_window.depth = 0;
    SaveState* state = (SaveState*)buffer;
    if (!SaveState::Load(state)) {
        // The engine now holds a partly restored timeline; re-simulating
        // from it would desync, so end the match instead (ledger A-001).
        AbortGgpoMatch(sf4e::loc::T("runtime.rollback_unsupported_state"));
        return false;
    }
    return true;
}

bool fSystem::ggpo_save_game_state_callback(unsigned char** buffer, int* len, int* checksum, int frame)
{
    GgpoCallbackScope _callbackScope;
    lastGgpoSaveFrame = frame;
    // No GGPO callback allocates data, then hands ownership to GGPO-
    // sf4e preallocates and manages all its savestates, and the memory
    // allocation all happens internally. Consequently the memory
    // utilization of _GGPO_ is technically zero- but GGPO
    // errors with an assertion if the length is zero.
    *len = 1;

    // After battle close GGPO still wants a buffer per resimulated frame.
    // Hand it an unused slot without saving; its later free is ignored.
    if (sf4e::NetplayFacade::DrainingSpectators()) {
        *buffer = (unsigned char*)&saveStates[0];
        *checksum = 0;
        return true;
    }

    // Find an empty position in our array, and store if we can
    // find one.
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used) {
            continue;
        }

        if (!SaveState::Save(&saveStates[i])) {
            // Save released the incomplete slot; GGPO must never load it.
            *buffer = nullptr;
            AbortGgpoMatch(sf4e::loc::T("runtime.rollback_unsupported_state"));
            return false;
        }
        CaptureNativeMatchResult(rSystem::staticMethods.GetSingleton(), frame);
        *buffer = (unsigned char*)&saveStates[i];
        *checksum = 0;

        // Preserve callback/engine frame identity for rollback diagnostics.
        // Semantic hashes are computed only by the separate periodic
        // checkpoint ring below; no consumer reads hashes from save slots.
        {
            rSystem* system = rSystem::staticMethods.GetSingleton();
            saveStates[i].simulationFrame =
                rSystem::GetNumFramesSimulated_FixedPoint(system)->integral;
            saveStates[i].ggpoFrame = frame;
        }

        if (diag::Enabled()) {
            uint32_t occupied = 0;
            for (int j = 0; j < NUM_SAVE_STATES; j++) {
                if (saveStates[j].used) {
                    occupied++;
                }
            }
            diag::G().occupiedSaveSlots.Update(occupied);
        }

        return true;
    }

    // No empty position in the array- either there aren't enough available
    // states, or the states aren't being released or tracked correctly.
    *buffer = nullptr;
    spdlog::error("FATAL: Could not store GGPO state!");
    AbortGgpoMatch(sf4e::loc::T("runtime.rollback_buffer_full"));
    return false;
}

bool fSystem::ggpo_log_game_state(char* filename, unsigned char* buffer, int)
{
    return true;
}

void fSystem::ggpo_free_buffer(void* buffer)
{
    GgpoCallbackScope _callbackScope;
    // GGPO hands back the pointer the save callback gave it, which is always
    // &saveStates[i]. Validate rather than trust: a stale or duplicated free
    // would otherwise run CopyIntoPlace on an arbitrary address and push
    // garbage keys into live engine objects.
    if (!buffer) {
        spdlog::error("GGPO: free_buffer called with null buffer; ignoring");
        return;
    }
    const char* base = (const char*)&saveStates[0];
    const char* target = (const char*)buffer;
    ptrdiff_t offset = target - base;
    if (offset < 0 ||
        offset >= (ptrdiff_t)(sizeof(SaveState) * NUM_SAVE_STATES) ||
        (offset % sizeof(SaveState)) != 0) {
        spdlog::error(
            "GGPO: free_buffer called with a pointer outside the savestate pool ({}); ignoring",
            buffer
        );
        return;
    }

    SaveState* victim = &saveStates[offset / sizeof(SaveState)];
    if (!victim->used) {
        // Battle close frees every slot while the engine is still alive. A
        // session kept open to drain spectators then hands them back on close.
        if (simGate.phase == sf4e::gate::PHASE_BATTLE_CLOSING) return;
        spdlog::error(
            "GGPO: free_buffer on slot {} which is already free; ignoring",
            offset / sizeof(SaveState)
        );
        return;
    }
    SaveState::Free(victim);
}

// True when the handle belongs to a spectator queue (any roster slot past
// the two fighters). Fighter handles are players[0..1]; a handle that
// matches neither fighter is treated as a spectator so a bogus handle can
// never end the fight.
static bool IsSpectatorHandle(GGPOPlayerHandle handle) {
    // A spectator client has one peer, the host stream; every event it sees
    // is about the link it depends on.
    if (fSystem::localPlayerHandle == GGPO_INVALID_HANDLE) {
        return false;
    }
    for (int i = 0; i < 2; i++) {
        if (fSystem::players[i].handle != GGPO_INVALID_HANDLE && fSystem::players[i].handle == handle) {
            return false;
        }
    }
    return true;
}

bool fSystem::ggpo_on_event_callback(GGPOEvent* info) {
    GgpoCallbackScope _callbackScope;
    rSystem* system = rSystem::staticMethods.GetSingleton();
    int progress;

    switch (info->code) {
    case GGPO_EVENTCODE_CONNECTED_TO_PEER:
        spdlog::info("GGPO: Connected!");
        sf4e::NetplayFacade::NotifyGgpoSyncPhase(sf4e::GgpoSyncPhase::Connected);
        break;
    case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
        progress = 100 * info->u.synchronizing.count / info->u.synchronizing.total;
        spdlog::info("GGPO: Synchronizing: {}", progress);
        sf4e::NetplayFacade::NotifyGgpoSyncPhase(sf4e::GgpoSyncPhase::Synchronizing);
        break;
    case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
        spdlog::info("GGPO: Synchronized with peer");
        s_spectatorPolicy.OnSynchronized(info->u.synchronized.player);
        break;
    case GGPO_EVENTCODE_RUNNING:
        s_spectatorPolicy.OnRunning();
        simGate.OnRunning();
        spdlog::info("GGPO: Running");
        sf4e::NetplayFacade::NotifyGgpoSyncPhase(sf4e::GgpoSyncPhase::Running);
        break;
    case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
        spdlog::info(
            "GGPO: GGPO_EVENTCODE_CONNECTION_INTERRUPTED player={} timeout_ms={}",
            info->u.connection_interrupted.player,
            info->u.connection_interrupted.disconnect_timeout
        );
        if (diag::Enabled()) {
            diag::G().OnConnectionInterrupted(diag::NowMs());
        }
        // A spectator's link is not the fight's link: note it, keep playing.
        // After the battle closed, P1 only drains spectators; the opponent
        // has already left the session, which is not a warning. The same
        // holds once the result is confirmed: the opponent may leave the
        // win screen first.
        if (IsSpectatorHandle(info->u.connection_interrupted.player) || sf4e::NetplayFacade::DrainingSpectators() ||
            NativeResultEmitted()) {
            break;
        }
        // Phase 2 behavior change: a connection warning marks quality
        // degraded but does NOT stop deterministic simulation. The game
        // keeps advancing while GGPO accepts local input, and stalls only
        // when the prediction threshold is reached. One alert per episode.
        s_disconnectTimeoutMs = info->u.connection_interrupted.disconnect_timeout;
        if (simGate.OnConnectionInterrupted(GetTickCount())) {
            sf4e::NetplayFacade::PushAlert(sf4e::loc::T("runtime.connection_unstable_prediction"), sf4e::NoticeSeverity::Warning);
        }
        break;
    case GGPO_EVENTCODE_CONNECTION_RESUMED:
        spdlog::info("GGPO: GGPO_EVENTCODE_CONNECTION_RESUMED player={}", info->u.connection_resumed.player);
        if (diag::Enabled()) {
            diag::G().OnConnectionResumed(diag::NowMs());
        }
        if (IsSpectatorHandle(info->u.connection_resumed.player)) {
            break;
        }
        // Clears only the warning. It cannot undo a manual pause, a fatal
        // transition, or the startup gate, and it never "catches up" by
        // double-advancing; GGPO resumes progression on its own.
        if (simGate.OnConnectionResumed()) {
            s_disconnectTimeoutMs = 0;
            sf4e::NetplayFacade::PushAlert(sf4e::loc::T("runtime.connection_restored"), sf4e::NoticeSeverity::Info);
        }
        break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
        spdlog::info("GGPO: GGPO_EVENTCODE_DISCONNECTED_FROM_PEER player={}", info->u.disconnected.player);
        if (diag::Enabled()) {
            diag::G().OnTerminalDisconnect(diag::NowMs());
        }
        // The fork raises this for spectator queues too. A spectator
        // dropping must not end the two fighters' game; GGPO has already
        // stopped forwarding to that spectator.
        if (IsSpectatorHandle(info->u.disconnected.player)) {
            s_spectatorPolicy.OnDisconnected(info->u.disconnected.player);
            spdlog::info("GGPO: spectator handle {} disconnected; fight continues", info->u.disconnected.player);
            if (!sf4e::NetplayFacade::DrainingSpectators())
                sf4e::NetplayFacade::PushAlert(sf4e::loc::T("runtime.spectator_disconnected"), sf4e::NoticeSeverity::Info);
            break;
        }
        // The fight already ended; the opponent closing its own session is expected.
        if (sf4e::NetplayFacade::DrainingSpectators()) {
            break;
        }
        if (system) {
            *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
        }
        simGate.OnConnectionResumed(); // close any open warning episode
        simGate.OnBattleClosing();     // the gate must not report RUNNING for a dead peer
        s_disconnectTimeoutMs = 0;
        if (!NativeResultEmitted()) {
            sf4e::NetplayFacade::PushAlert(
                sf4e::loc::T(localPlayerHandle == GGPO_INVALID_HANDLE ? "runtime.match_connection_lost" : "runtime.opponent_disconnected"),
                sf4e::NoticeSeverity::Error
            );
        }
        break;
    case GGPO_EVENTCODE_TIMESYNC:
        if (diag::Enabled()) {
            diag::G().OnTimesyncEvent(info->u.timesync.frames_ahead);
        }
        // No blocking here: the pacer records the recommendation and
        // StepPacing repays it outside this callback. In continuous mode the
        // pacer only counts it, since the rift samples already cover it.
        pacer.OnRecommendation(info->u.timesync.frames_ahead);
        spdlog::info(
            "GGPO: timesync recommends {} frames; outstanding pacing {:.1f} ms",
            info->u.timesync.frames_ahead,
            pacer.outstandingMs
        );
        break;
    default:
        spdlog::warn("GGPO: unhandled event code {}", (int)info->code);
        break;
    }
    return true;
}
