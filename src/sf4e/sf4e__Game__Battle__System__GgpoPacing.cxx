// sf4e::Game::Battle::System: rift pacing, the netplay time series and match
// telemetry. Split from sf4e__Game__Battle__System__Ggpo.cxx.
#include "sf4e__Game__Battle__System__Internal.hxx"
#include <atomic>

NetplayWindow s_window;
static constexpr uint64_t kNetplayWindowMs = 15000;

void NoteSpectatorBacklog(int backlogFrames, int catchUpFrames) {
    fSystem::matchTelemetry.backlog = backlogFrames;
    s_window.spectator.Sample(backlogFrames);
    s_window.spectator.CaughtUp(catchUpFrames);
}

// Applies development overrides for the pacing caps and resets the
// controller for a new session. Called from StartGGPO/StartSpectating.
void ResetPacerForSession() {
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

unsigned fSystem::RecentRollbackFrames() { return rollbackHud.Recent(GetTickCount64()); }
sf4e::MatchTelemetry fSystem::matchTelemetry;
void fSystem::PollMatchTelemetry() {
    if (!ggpo) return;
    const auto now = GetTickCount64();
    // A spectator has no local fighter handle; do not label its host link as fighter RTT.
    matchTelemetry.spectator = !LocalFighter();
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
    // Beside the overlay's thread note: whether the simulation shares a thread
    // with the window messages, which wait on the overlay's ImGui lock.
    static std::atomic<bool> threadNoted{ false };
    if (!threadNoted.exchange(true)) spdlog::info("GGPO: battle ticks run on thread {}", GetCurrentThreadId());
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

