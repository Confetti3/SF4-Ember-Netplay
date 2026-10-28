// sf4e::Game::Battle::System: the GGPO session callbacks (begin, advance,
// load, save, free, log and events). Split from
// sf4e__Game__Battle__System__Ggpo.cxx.
#include "sf4e__Game__Battle__System__Internal.hxx"

// Stands in for a saved state on frames that save nothing (the save callback).
static unsigned char s_inertSaveState;

bool fSystem::ggpo_begin_game_callback(const char*)
{
    return true;
}

bool fSystem::ggpo_advance_frame_callback(int)
{
    GgpoCallbackScope _callbackScope;
    diag::ScopedTimer _cbTimer(diag::OP_ROLLBACK_CALLBACK);
    if (diag::Enabled()) {
        diag::G().OnRollbackCallback(diag::NowMs());
    }

    if (!ggpo) {
        return true;
    }
    // Once the session is fatal (an earlier callback in this burst aborted),
    // GGPO still calls back for the remaining frames; once the battle is
    // closed, a session kept open for spectators may still resimulate on a
    // late input. Neither touches the engine, but both keep GGPO's frame
    // count moving: a rollback that ends short of its target fails GGPO's
    // frame-count ASSERT, which calls exit(1) in release builds too.
    if (CallbacksInert()) {
        ggpo_advance_frame(ggpo);
        return true;
    }

    fPadSystem::Inputs inputs[2] = { {0, 0}, {0, 0} };
    int disconnect_flags = 0;

    // Make sure we fetch new inputs from GGPO and use those to update
    // the game state instead of reading from the selected input device.
    GGPOErrorCode result = ReadGgpoInputs(inputs, &disconnect_flags, FramePass::Resim);
    if (!GGPO_SUCCEEDED(result)) {
        AbortGgpoMatch(sf4e::loc::T("runtime.netplay_sync_failed"));
        if (ggpo) ggpo_advance_frame(ggpo);
        return true;
    }
    NoteDisconnectFlags(disconnect_flags);

    // The same step as a frame of the present (PlayGgpoFrame), on the
    // undetoured engine update, without the pre-step sound sync. Playback
    // mode ends with the engine update there too, so this frame's sample and
    // save see the pads as the live frame's do.
    rSystem* system = rSystem::staticMethods.GetSingleton();
    result = SimulateGgpoFrame(system, inputs, FramePass::Resim);
    if (!GGPO_SUCCEEDED(result)) {
        AbortGgpoMatch(sf4e::loc::T("runtime.netplay_sync_failed"));
    }
    else {
        rollbackHud.Replayed(GetTickCount64());
        ++s_window.resimFrames;
        s_window.maxDepth = (std::max)(s_window.maxDepth, ++s_window.depth);
        CaptureSnapshot(system);
        CaptureMeterSample(system, false);
    }

    return true;
}

bool fSystem::ggpo_load_game_state_callback(unsigned char* buffer, int len)
{
    GgpoCallbackScope _callbackScope;
    if (CallbacksInert()) {
        return true;
    }
    rollbackHud.Begin(GetTickCount64());
    ++s_window.rollbacks;
    s_window.depth = 0;
    SaveState* state = (SaveState*)buffer;
    if (!ggpoTimeline.Load(state->ggpoFrame, [&] { return SaveState::Load(state); })) {
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
    ggpoTimeline.Saved(frame);
    // No GGPO callback allocates data, then hands ownership to GGPO-
    // sf4e preallocates and manages all its savestates, and the memory
    // allocation all happens internally. Consequently the memory
    // utilization of _GGPO_ is technically zero- but GGPO
    // errors with an assertion if the length is zero.
    *len = 1;

    // A fatal session (until the outer tick closes it) and a battle already
    // closed for draining spectators only move GGPO's frame count; nothing is
    // saved. GGPO still needs a buffer per frame, and asserts one exists if a
    // later rollback in the same poll loads it, so these frames all share the
    // inert token: loading it touches nothing in either state, and freeing it
    // is ignored.
    if (CallbacksInert()) {
        *buffer = &s_inertSaveState;
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
    if (buffer == &s_inertSaveState) return;
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
    if (!fSystem::LocalFighter()) {
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
        // The spectator sync deadline only starts once a fighter is synchronized.
        if (IsSpectatorHandle(info->u.synchronized.player)) {
            s_spectatorPolicy.OnSynchronized(info->u.synchronized.player);
        } else {
            s_spectatorPolicy.OnFighterSynchronized(GetTickCount64());
        }
        break;
    case GGPO_EVENTCODE_RUNNING:
        s_spectatorPolicy.OnRunning(GetTickCount64());
        s_streamWatch.OnRunning();
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
        if (IsSpectatorHandle(info->u.connection_interrupted.player) || drainingSpectators ||
            MatchResultKnown()) {
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
        // stopped forwarding to that spectator. The fighters are not told:
        // the HUD's watcher count already drops.
        if (IsSpectatorHandle(info->u.disconnected.player)) {
            s_spectatorPolicy.OnDisconnected(info->u.disconnected.player);
            s_spectatorLinks.Dropped(info->u.disconnected.player);
            spdlog::info("GGPO: spectator handle {} disconnected; fight continues", info->u.disconnected.player);
            break;
        }
        // The fight already ended; the opponent closing its own session is expected.
        if (drainingSpectators) {
            break;
        }
        if (system) {
            *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
        }
        simGate.OnConnectionResumed(); // close any open warning episode
        simGate.OnBattleClosing();     // the gate must not report RUNNING for a dead peer
        s_disconnectTimeoutMs = 0;
        if (!MatchResultKnown()) {
            sf4e::NetplayFacade::PushAlert(
                sf4e::loc::T(!LocalFighter() ? "runtime.spectator_stream_lost" : "runtime.opponent_disconnected"),
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
