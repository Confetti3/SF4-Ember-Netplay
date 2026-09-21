#include "sf4e__Game__Battle__System__Internal.hxx"

static sf4e::RollbackHud rollbackHud;

// Last disconnect_flags observed from ggpo_synchronize_input; logged on
// change for diagnostics only (no gameplay semantics attached).
static int s_lastDisconnectFlags = 0;

// GGPO callback re-entrancy. ggpo_close_session deletes the backend, and the
// fork keeps calling the advance-frame callback from Sync::AdjustSimulation
// after the callback returns, so the session must never be closed from inside
// a callback. Aborts raised while a callback is on the stack are latched here
// and drained by DrainPendingAbort() once the top-level GGPO call returns.
static sf4e::gate::AbortLatch s_abortLatch;

struct GgpoCallbackScope : sf4e::gate::AbortLatch::Scope {
    GgpoCallbackScope() : sf4e::gate::AbortLatch::Scope(s_abortLatch) {}
};

// Native result state is captured in GGPO saves, including resimulation. The
// history is rewound to each restored state; the emitted latch is deliberately
// match state and survives a rollback so a
// result cannot be submitted twice after re-simulation.
sf4e::native_result::Timeline s_nativeResultTimeline;
static bool s_nativeResultEmitted = false;

static void ResetNativeResultMatch() {
    s_nativeResultTimeline.Reset();
    s_nativeResultEmitted = false;
}

static void CaptureNativeMatchResult(rSystem* system, int stateFrame) {
    if (!system || !rSystem::staticVars.CurrentBattleFlow) {
        s_nativeResultTimeline.Capture(stateFrame, sf4e::native_result::Flow::Other, -1);
        return;
    }

    const DWORD flowValue = *rSystem::staticVars.CurrentBattleFlow;
    sf4e::native_result::Flow flow = sf4e::native_result::Flow::Other;
    int winnerIndex = -1;
    if (flowValue == rSystem::BF__MATCH_RESULT) {
        flow = sf4e::native_result::Flow::MatchResult;
        GameManager* manager = (system->*rSystem::publicMethods.GetGameManager)();
        if (!manager || !GameManager::publicMethods.GetNativeResultIndex) {
            s_nativeResultTimeline.Capture(stateFrame, sf4e::native_result::Flow::Other, -1);
            return;
        }

        // The native result flow (SSFIV.exe absolute VA 0x005DD93A, RVA
        // 0x001DD93A; GameManager vtable +0x30) uses -1 for BF_DRAW_RESULT and
        // a player index for BF_MATCH_RESULT.  This is the match-level field
        // written by the native round-score finalizer; the separate +0x4C field is the
        // current round winner used by the result HUD and is not used here.
        winnerIndex = (manager->*GameManager::publicMethods.GetNativeResultIndex)();
    }
    else if (flowValue == rSystem::BF__DRAW_RESULT) {
        flow = sf4e::native_result::Flow::DrawResult;
    }

    s_nativeResultTimeline.Capture(stateFrame, flow, winnerIndex);
}

static void PublishConfirmedNativeMatchResult() {
    if (s_nativeResultEmitted || !fSystem::ggpo ||
        fSystem::localPlayerHandle == GGPO_INVALID_HANDLE) return;
    int confirmed = -1;
    if (!GGPO_SUCCEEDED(ggpo_get_last_confirmed_frame(fSystem::ggpo, &confirmed))) return;
    const auto result = s_nativeResultTimeline.Confirmed(confirmed);
    if (result == sf4e::native_result::Result::None) return;

    s_nativeResultEmitted = true;
    spdlog::info("Match result: confirmed native outcome={} input_frame={}", static_cast<int>(result), confirmed);
    switch (result) {
    case sf4e::native_result::Result::P1Win:
        sf4e::NetplayFacade::NotifyRuntimeMatchResult(sf4e::room::MatchResult::P1Win);
        break;
    case sf4e::native_result::Result::P2Win:
        sf4e::NetplayFacade::NotifyRuntimeMatchResult(sf4e::room::MatchResult::P2Win);
        break;
    case sf4e::native_result::Result::Draw:
        sf4e::NetplayFacade::NotifyRuntimeMatchResult(sf4e::room::MatchResult::Draw);
        break;
    case sf4e::native_result::Result::None:
        break;
    }
}

// The frame loop only publishes after a successful advance. Once the fight
// has stopped simulating, confirmation of the result frames can still arrive
// through a plain GGPO poll; without this entry that outcome was never sent.
void fSystem::PollNativeMatchResult() {
    PublishConfirmedNativeMatchResult();
}

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


static void NoteDisconnectFlags(int flags) {
    if (flags != s_lastDisconnectFlags) {
        spdlog::info(
            "GGPO: disconnect_flags changed {:#x} -> {:#x}",
            s_lastDisconnectFlags,
            flags
        );
        s_lastDisconnectFlags = flags;
    }
}


// Emits the complete diagnostics summary through the normal log. Called at
// match end / abort, before battle state is torn down.
void EmitRollbackDiagSummary(const char* label) {
    if (!diag::Enabled()) {
        return;
    }
    static char s_diagBuf[16384];
    size_t n = diag::G().FormatSummary(s_diagBuf, sizeof(s_diagBuf), label);
    if (n) {
        spdlog::info("\n{}", s_diagBuf);
    }
}

bool fSystem::bHaltAfterNext = false;
bool fSystem::bUpdateAllowed = true;

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
int fSystem::nExtraFramesToSimulate = 0;
int fSystem::nNextBattleStartFlowTarget = -1;
int fSystem::nRandomizeLocalInputsEveryXFramesInGGPO = 0;

GGPOPlayerHandle fSystem::localPlayerHandle = GGPO_INVALID_HANDLE;
int fSystem::lastGgpoSaveFrame = -1;
GGPOSession* fSystem::ggpo = nullptr;
sf4e::gate::GgpoGateModel fSystem::simGate = { sf4e::gate::PHASE_NO_SESSION };
// Zeroed (so disabled) until ResetPacerForSession initializes it.
sf4e::pacing::PacingController fSystem::pacer;
bool fSystem::continuousTimesync = true;

// Applies development overrides for the pacing caps and resets the
// controller for a new session. Called from StartGGPO/StartSpectating.
static void ResetPacerForSession() {
    fSystem::pacer.InitDefaults();
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
    fSystem::continuousTimesync = sf4e::EnvFlag("SF4E_CONTINUOUS_TIMESYNC", true);
    spdlog::info("Netplay experiments: continuousTimesync={} inputRepair={}",
        fSystem::continuousTimesync, sf4e::EnvFlag("SF4E_GGPO_INPUT_REPAIR", true));
}

static void LogPacerSummary(const char* label) {
    const sf4e::pacing::PacingController& p = fSystem::pacer;
    if (p.recommendationsReceived == 0 && p.msAppliedTotal == 0.0 && p.riftSamples == 0) {
        return;
    }
    spdlog::info(
        "Pacing [{}]: enabled={} recs={} framesRec={} acceptedMs={:.1f} "
        "replacedMs={:.1f} disabledDiscardMs={:.1f} resetDiscardMs={:.1f} "
        "waits={} requestedMs={:.1f} actualMs={:.1f} maxRequestedMs={:.2f} "
        "maxActualMs={:.2f} failures={} timeouts={} fallbacks={} "
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
        p.waitRequests,
        p.msRequestedTotal,
        p.msAppliedTotal,
        p.maxRequestedWaitMs,
        p.maxSingleWaitMs,
        p.waitFailures,
        p.waitTimeouts,
        p.fallbackSleeps,
        p.maxOutstandingMs,
        p.outstandingMs,
        fSystem::continuousTimesync,
        p.riftSamples,
        p.msRiftAcceptedTotal,
        p.riftFramesEma,
        p.maxAbsRiftFrames
    );
}

bool fSystem::MayAdvanceDeterministicFrame() {
    // Preserve the legacy Boolean for offline/development controls, but make
    // the explicit model authoritative for GGPO lifecycle and terminal state.
    // Connection warnings and prediction stalls intentionally remain absent.
    return !ggpo
        ? bUpdateAllowed
        : bUpdateAllowed && simGate.CanAdvanceDeterministicFrame();
}
fSystem::PlayerConnectionInfo fSystem::players[sf4e::room::MaxMatchParticipants];
fSystem::SaveState fSystem::saveStates[NUM_SAVE_STATES];

rKey::MementoID GGPO_MEMENTO_ID = { 1, 1 };

bool fSystem::extendedLoadRequest = false;
bool fSystem::extendedSaveRequest = false;
GameMementoKey::MementoID fSystem::mementoLoadRequest = { 0xffffffff, 0xffffffff };
GameMementoKey::MementoID fSystem::mementoSaveRequest = { 0xffffffff, 0xffffffff };

void fSystem::Install() {
    void (fSystem:: * _fBattleUpdate)() = &BattleUpdate;
    void (fSystem:: * _fCloseBattle)() = &CloseBattle;
    void (fSystem:: * _fSysMain_HandleTrainingModeFeatures)() = &SysMain_HandleTrainingModeFeatures;
    void (fSystem:: * _fSysMain_UpdatePauseState)() = &SysMain_UpdatePauseState;
    int (fSystem:: * _fGetMementoSize)() = &GetMementoSize;
    int (fSystem:: * _fRecordToMemento)(Memento * m, GameMementoKey::MementoID * id) = &RecordToMemento;
    int (fSystem:: * _fRestoreFromMemento)(Memento * m, GameMementoKey::MementoID * id) = &RestoreFromMemento;

    DetourAttach((PVOID*)&rSystem::mementoableMethods.GetMementoSize, *(PVOID*)&_fGetMementoSize);
    DetourAttach((PVOID*)&rSystem::mementoableMethods.RecordToMemento, *(PVOID*)&_fRecordToMemento);
    DetourAttach((PVOID*)&rSystem::mementoableMethods.RestoreFromMemento, *(PVOID*)&_fRestoreFromMemento);

    DetourAttach((PVOID*)&rSystem::publicMethods.BattleUpdate, *(PVOID*)&_fBattleUpdate);
    DetourAttach((PVOID*)&rSystem::publicMethods.CloseBattle, *(PVOID*)&_fCloseBattle);
    DetourAttach((PVOID*)&rSystem::publicMethods.SysMain_HandleTrainingModeFeatures, *(PVOID*)&_fSysMain_HandleTrainingModeFeatures);
    DetourAttach((PVOID*)&rSystem::publicMethods.SysMain_UpdatePauseState, *(PVOID*)&_fSysMain_UpdatePauseState);
    DetourAttach((PVOID*)&rSystem::staticMethods.OnBattleFlow_BattleStart, OnBattleFlow_BattleStart);
}

int fSystem::GetMementoSize() {
    return (this->*rSystem::mementoableMethods.GetMementoSize)() + sizeof(AdditionalMemento);
}

int fSystem::RecordToMemento(Memento* m, GameMementoKey::MementoID* id) {
    AdditionalMemento* additional = (AdditionalMemento*)((unsigned int)m + sizeof(Memento));
    rSystem* _this = rSystem::FromMementoable(this);
    additional->nFirstCharaToSimulate = *rSystem::GetFirstCharaToSimulate(_this);
    additional->skipRelatedFlags_0xd8c = *rSystem::GetSkipRelatedFlags_0xd8c(_this);
    additional->simulationFlags = *rSystem::GetSimulationFlags(_this);
    additional->transitionProgress  = *rSystem::GetTransitionProgress(_this);
    additional->transitionSpeed = *rSystem::GetTransitionSpeed(_this);
    additional->transitionType = *rSystem::GetTransitionType(_this);
    additional->network = *(NetworkUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_NETWORK);

    HudUnit* hud = (HudUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_HUD);
    fHud::Announce::Unit::RecordToAdditionalMemento(*HudUnit::GetAnnounce(hud), additional->announce);

    rHud::Notice::View* noticeView = *rHud::Notice::Unit::GetView(*HudUnit::GetNotice(hud));
    WithReleaser<rHud::Notice::Player>* noticePlayers = rHud::Notice::View::GetPlayers(noticeView);
    for (int playerIdx = 0; playerIdx < (_this->*rSystem::publicMethods.GetNumCharasToSimulateThisFrame)(); playerIdx++) {
        fHud::Notice::Player::RecordToAdditionalMemento(
            noticePlayers[playerIdx].obj,
            additional->playerNotices[playerIdx]
        );
    }

    Platform::GFxApp::RecordToAdditionalMemento(
        Dimps::Platform::GFxApp::staticMethods.GetSingleton(),
        additional->gfxApp
    );

    Eva::TaskCore::RecordToAdditionalMemento(
        (_this->*rSystem::publicMethods.GetTaskCore)(System::TCI_UPDATE),
        additional->updateCore
    );

    return (this->*rSystem::mementoableMethods.RecordToMemento)(m, id);
}

int fSystem::RestoreFromMemento(Memento* m, GameMementoKey::MementoID* id) {
    AdditionalMemento* additional = (AdditionalMemento*)((unsigned int)m + sizeof(Memento));
    rSystem* _this = rSystem::FromMementoable(this);
    *rSystem::GetFirstCharaToSimulate(_this) = additional->nFirstCharaToSimulate;
    *rSystem::GetSkipRelatedFlags_0xd8c(_this) = additional->skipRelatedFlags_0xd8c;
    *rSystem::GetSimulationFlags(_this) = additional->simulationFlags;
    *rSystem::GetTransitionProgress(_this) = additional->transitionProgress;
    *rSystem::GetTransitionSpeed(_this) = additional->transitionSpeed;
    *rSystem::GetTransitionType(_this) = additional->transitionType;
    *(NetworkUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_NETWORK) = additional->network;

    HudUnit* hud = (HudUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_HUD);
    rHud::Announce::Unit* announce = *HudUnit::GetAnnounce(hud);
    fHud::Announce::Unit::RestoreFromAdditionalMemento(announce, additional->announce);

    rHud::Notice::View* noticeView = *rHud::Notice::Unit::GetView(*HudUnit::GetNotice(hud));
    WithReleaser<rHud::Notice::Player>* noticePlayers = rHud::Notice::View::GetPlayers(noticeView);
    for (int playerIdx = 0; playerIdx < (_this->*rSystem::publicMethods.GetNumCharasToSimulateThisFrame)(); playerIdx++) {
        fHud::Notice::Player::RestoreFromAdditionalMemento(
            noticePlayers[playerIdx].obj,
            additional->playerNotices[playerIdx]
        );
    }

    Platform::GFxApp::RestoreFromAdditionalMemento(
        Dimps::Platform::GFxApp::staticMethods.GetSingleton(),
        additional->gfxApp
    );

    Dimps::Eva::TaskCore* updateCore = (_this->*rSystem::publicMethods.GetTaskCore)(System::TCI_UPDATE);
    Eva::TaskCore::RestoreFromAdditionalMemento(updateCore, additional->updateCore);

    // Now that the task core is restored, update all the handles.
    CameraUnit* cam = (CameraUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(U_CAMERA);
    PauseUnit* pause = (PauseUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(U_PAUSE);
    *PauseUnit::GetPauseTask(pause) = nullptr;
    *CameraUnit::GetCamShakeTask(cam) = nullptr;
    *rHud::Announce::Unit::GetHudAnnounceUpdateTask(announce) = nullptr;
    *rHud::Cockpit::Unit::GetHudCockpitUpdateTask(*HudUnit::GetCockpit(hud)) = nullptr;
    if (*HudUnit::GetContinue(hud)) {
        *rHud::Continue::Unit::GetHudContinueUpdateTask(*HudUnit::GetContinue(hud)) = nullptr;
    }
    *rHud::Cursor::Unit::GetHudCursorUpdateTask(*HudUnit::GetCursor(hud)) = nullptr;
    *rHud::Notice::Unit::GetHudNoticeUpdateTask(*HudUnit::GetNotice(hud)) = nullptr;
    if (*HudUnit::GetResult(hud)) {
        *rHud::Result::Unit::GetHudResultUpdateTask(*HudUnit::GetResult(hud)) = nullptr;
    }
    if (*HudUnit::GetSubtitle(hud)) {
        *rHud::Subtitle::Unit::GetHudSubtitleUpdateTask(*HudUnit::GetSubtitle(hud)) = nullptr;
    }
    if (*HudUnit::GetTraining(hud)) {
        *rHud::Training::Unit::GetHudTrainingUpdateTask(*HudUnit::GetTraining(hud)) = nullptr;
    }

    Dimps::Eva::Task* cursor;
    for (
        cursor = Dimps::Eva::TaskCore::GetTaskHead(updateCore);
        cursor != nullptr;
        cursor = *Dimps::Eva::Task::GetNext(cursor)
    ) {
        char* name = (updateCore->*Dimps::Eva::TaskCore::publicMethods.GetTaskName)(&cursor);
        if (strcmp(name, "PAUSE") == 0) {
            *PauseUnit::GetPauseTask(pause) = cursor;
        } else if (strcmp(name, "CAM SHAKE") == 0) {
            *CameraUnit::GetCamShakeTask(cam) = cursor;
        }
        else if (strcmp(name, "HUD ANNOUNCE") == 0) {
            *rHud::Announce::Unit::GetHudAnnounceUpdateTask(announce) = cursor;
        }
        else if (strcmp(name, "HUD COCKPIT") == 0) {
            *rHud::Cockpit::Unit::GetHudCockpitUpdateTask(*HudUnit::GetCockpit(hud)) = cursor;
        }
        else if (strcmp(name, "HUD CONTINUE") == 0) {
            *rHud::Continue::Unit::GetHudContinueUpdateTask(*HudUnit::GetContinue(hud)) = cursor;
        }
        else if (strcmp(name, "HUD CURSOR") == 0) {
            *rHud::Cursor::Unit::GetHudCursorUpdateTask(*HudUnit::GetCursor(hud)) = cursor;
        }
        else if (strcmp(name, "HUD NOTICE") == 0) {
            *rHud::Notice::Unit::GetHudNoticeUpdateTask(*HudUnit::GetNotice(hud)) = cursor;
        }
        else if (strcmp(name, "HUD RESULT") == 0) {
            *rHud::Result::Unit::GetHudResultUpdateTask(*HudUnit::GetResult(hud)) = cursor;
        }
        else if (strcmp(name, "HUD SUBTITLE") == 0) {
            *rHud::Subtitle::Unit::GetHudSubtitleUpdateTask(*HudUnit::GetSubtitle(hud)) = cursor;
        }
        else if (strcmp(name, "HUD TRAINING") == 0) {
            if (*HudUnit::GetTraining(hud)) {
                *rHud::Training::Unit::GetHudTrainingUpdateTask(*HudUnit::GetTraining(hud)) = nullptr;
            }
        }
    }

    const int result = (this->*rSystem::mementoableMethods.RestoreFromMemento)(m, id);
    return result;
}


void fSystem::BattleUpdate() {
    rSystem* _this = (rSystem*)this;
    rSystem::__publicMethods& sysMethods = rSystem::publicMethods;
    rPadSystem* p = rPadSystem::staticMethods.GetSingleton();
    rPadSystem::__publicMethods& padMethods = rPadSystem::publicMethods;
    static int nLastRandomInputFrame = -1;
    static fPadSystem::Inputs randomInputs = { 0, 0 };

    diag::ScopedTimer _updateTimer(diag::OP_DETOURED_BATTLE_UPDATE);

    if (!MayAdvanceDeterministicFrame()) {
        if (ggpo && diag::Enabled()) {
            diag::G().RecordSkip(diag::SKIP_UPDATE_GATE, diag::NowMs());
        }
        return;
    }

    if (ggpo && *rSystem::staticVars.CurrentBattleFlow != BF__IDLE) {
        // Pump the network right before the inputs are needed. The outer
        // tick's poll runs after this frame was rendered, so without this
        // pump any remote input that arrived since then is used one frame
        // late and predicted for one frame more than the link requires. A
        // rollback triggered here is the same rollback the outer poll would
        // have run, just before the frame that needs it.
        {
            diag::ScopedTimer _t(diag::OP_GGPO_IDLE_PRE_SIM);
            ggpo_idle(ggpo, 0);
        }
        if (DrainPendingAbort() || !ggpo || !MayAdvanceDeterministicFrame()) {
            return;
        }
        GGPOErrorCode result = GGPO_OK;
        if (localPlayerHandle != GGPO_INVALID_HANDLE) {
            for (int i = 0; i < 2; i++) {
                if (players[i].type == GGPO_PLAYERTYPE_LOCAL) {
                    fPadSystem::Inputs inputs;
                    if (nRandomizeLocalInputsEveryXFramesInGGPO != 0) {
                        int currentFrame = rSystem::GetNumFramesSimulated_FixedPoint(_this)->integral;
                        if (
                            nLastRandomInputFrame < 0 ||
                            (currentFrame - nLastRandomInputFrame) > nRandomizeLocalInputsEveryXFramesInGGPO
                        ) {
                            randomInputs = { localRand(), localRand() };
                            nLastRandomInputFrame = currentFrame;
                        }
                        inputs = randomInputs;
                    }
                    else {
                        if(sf4e::NetplayFacade::IsRuntimeRoomActive())
                            sf4e::NetplayFacade::ReadRuntimeMatchInput(i,inputs.mappedOn,inputs.rawOn);
                        else inputs = { (p->*padMethods.GetButtons_MappedOn)(i), (p->*padMethods.GetButtons_RawOn)(i) };
                    }
                    {
                        diag::ScopedTimer _t(diag::OP_ADD_LOCAL_INPUT);
                        result = ggpo_add_local_input(ggpo, players[i].handle, &inputs, sizeof(fPadSystem::Inputs));
                    }
                    if (diag::Enabled()) {
                        diag::G().RecordGgpoResult(diag::CALL_ADD_LOCAL_INPUT, (int)result);
                    }
                    break;
                }
            }
        }
        if (DrainPendingAbort()) {
            return;
        }

        switch (sf4e::gate::ClassifyGgpoResult((int)result)) {
        case sf4e::gate::POLICY_CONTINUE:
            break;
        case sf4e::gate::POLICY_STALL_PREDICTION:
            // GGPO refuses further prediction. Do not synchronize, do not
            // run the engine update, do not advance for this outer frame.
            simGate.OnPredictionThreshold();
            if (diag::Enabled()) {
                diag::G().RecordSkip(diag::SKIP_PREDICTION_THRESHOLD, diag::NowMs());
            }
            return;
        case sf4e::gate::POLICY_SKIP_NOT_SYNCED:
            if (diag::Enabled()) {
                diag::G().RecordSkip(diag::SKIP_NOT_SYNCHRONIZED, diag::NowMs());
            }
            return;
        case sf4e::gate::POLICY_SKIP_OTHER:
            spdlog::warn("GGPO: add_local_input returned {}; skipping frame", (int)result);
            if (diag::Enabled()) {
                diag::G().RecordSkip(diag::SKIP_ADD_INPUT_ERROR, diag::NowMs());
            }
            return;
        case sf4e::gate::POLICY_FATAL:
        default:
            spdlog::error("GGPO: add_local_input returned irrecoverable {}", (int)result);
            AbortGgpoMatch(sf4e::loc::T("runtime.netplay_input_failed"));
            return;
        }

        {
            fPadSystem::Inputs ggpoInputs[2] = { {0, 0}, {0, 0} };
            int disconnect_flags = 0;
            {
                diag::ScopedTimer _t(diag::OP_SYNC_INPUT);
                result = ggpo_synchronize_input(ggpo, (void*)ggpoInputs, sizeof(fPadSystem::Inputs) * 2, &disconnect_flags);
            }
            if (diag::Enabled()) {
                diag::G().RecordGgpoResult(diag::CALL_SYNC_INPUT, (int)result);
            }
            if (DrainPendingAbort()) {
                return;
            }
            switch (sf4e::gate::ClassifyGgpoResult((int)result)) {
            case sf4e::gate::POLICY_CONTINUE:
                break;
            case sf4e::gate::POLICY_FATAL:
                spdlog::error("GGPO: synchronize_input returned irrecoverable {}", (int)result);
                AbortGgpoMatch(sf4e::loc::T("runtime.netplay_sync_failed"));
                return;
            default:
                // NOT_SYNCHRONIZED during startup/resync, or another
                // transient refusal: skip this frame without simulating.
                if (diag::Enabled()) {
                    diag::G().RecordSkip(diag::SKIP_SYNC_INPUT_ERROR, diag::NowMs());
                }
                return;
            }
            NoteDisconnectFlags(disconnect_flags);
            {
                PlaybackFrameScopeGuard _playbackGuard;
                fPadSystem::playbackFrame = 0;
                fPadSystem::playbackData[0][0] = ggpoInputs[0];
                fPadSystem::playbackData[0][1] = ggpoInputs[1];
                if (fSoundPlayerManager::bUsePureSounds) {
                    fSoundPlayerManager::SyncState();
                }
                {
                    diag::ScopedTimer _t(diag::OP_ENGINE_BATTLE_UPDATE);
                    (_this->*sysMethods.BattleUpdate)();
                }
                // Playback mode must be off before ggpo_advance_frame: the
                // save callback and any nested GGPO work must not read the
                // stale playback inputs. The guard also restores on every
                // early exit above.
                fPadSystem::playbackFrame = -1;
            }
            GGPOErrorCode err;
            {
                diag::ScopedTimer _t(diag::OP_ADVANCE_FRAME_API);
                err = ggpo_advance_frame(ggpo);
            }
            if (diag::Enabled()) {
                diag::G().RecordGgpoResult(diag::CALL_ADVANCE_FRAME, (int)err);
            }
            if (DrainPendingAbort()) {
                return;
            }
            if (!GGPO_SUCCEEDED(err)) {
                spdlog::error("GGPO: advance_frame returned {}", (int)err);
                AbortGgpoMatch(sf4e::loc::T("runtime.netplay_sync_failed"));
            }
            else {
                simGate.OnFrameAccepted();
                if (diag::Enabled()) {
                    diag::G().OnFrameAdvanced(diag::NowMs());
                }
                if (fSoundPlayerManager::bUsePureSounds) {
                    fSoundPlayerManager::SyncState();
                }
                CaptureSnapshot(_this);
                CaptureHashCheckpoint(_this);
                PublishConfirmedNativeMatchResult();
            }
        }
    }
    else {
        if (fSoundPlayerManager::bUsePureSounds) {
            fSoundPlayerManager::SyncState();
        }
        sf4e::training::BeforeUpdate(_this, ggpo != nullptr);
        // The stress harness owns the memento keys while it runs. This branch
        // is also taken with a live session while the flow is idle, so it
        // must never run alongside GGPO's own pool.
        if (ggpo || !StressStep(_this)) {
            (_this->*rSystem::publicMethods.BattleUpdate)();
        }
        sf4e::training::AfterUpdate(_this);
    }

    if (nExtraFramesToSimulate > 0) {
        for (int i = 0; i < nExtraFramesToSimulate; i++) {
            fPadSystem::playbackFrame = i;
            (_this->*sysMethods.BattleUpdate)();
        }
        fPadSystem::playbackFrame = -1;
        nExtraFramesToSimulate = 0;
    }

    if (bHaltAfterNext) {
        bHaltAfterNext = false;
        bUpdateAllowed = false;
        simGate.SetManualPause(true);
    }
}

// Emits the occupancy of the savestate pool. Called on entry to every
// teardown/startup path so the log alone distinguishes "the pool was already
// corrupt when we got here" from "this teardown corrupted it", without
// needing a crash dump.
void fSystem::LogSaveSlotOccupancy(const char* label) {
    uint32_t occupied = 0;
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used || !saveStates[i].keys.empty()) {
            occupied++;
            spdlog::info(
                "SaveSlots [{}]: slot {} used={} keys={} ownsKeys={} simFrame={} ggpoFrame={}",
                label,
                i,
                saveStates[i].used,
                saveStates[i].keys.size(),
                saveStates[i].ownsKeys,
                saveStates[i].simulationFrame,
                saveStates[i].ggpoFrame
            );
        }
    }
    spdlog::info("SaveSlots [{}]: {}/{} occupied", label, occupied, NUM_SAVE_STATES);
}

void fSystem::CloseBattle() {
    rSystem* _this = (rSystem*)this;
    sf4e::training::CloseBattle();
    bool summaryEmitted = false;
    LogSaveSlotOccupancy("battle_close_entry");
    if (ggpo) {
        PublishConfirmedNativeMatchResult();
        int confirmedInput = -1;
        ggpo_get_last_confirmed_frame(ggpo, &confirmedInput);
        const auto candidate = s_nativeResultTimeline.Latest();
        spdlog::info(
            "Match result: native teardown outcome_emitted={} candidate={} candidate_frame={} confirmed_input={} last_save_frame={}",
            s_nativeResultEmitted,
            static_cast<int>(candidate.result),
            candidate.frame,
            confirmedInput,
            lastGgpoSaveFrame
        );
        simGate.OnBattleClosing();
        // Decide defer *before* close so a prior spectator defer flag cannot
        // leave this session open across rematch.
        sf4e::NetplayFacade::NotifyMatchEnded();
        if (!sf4e::NetplayFacade::ShouldDeferGgpoClose()) {
            RetireGgpoSession("battle_close");
            summaryEmitted = true;
        }
        else {
            if (diag::Enabled()) {
                diag::G().OnSessionEnded(diag::NowMs());
            }
            LogPacerSummary("battle_close_deferred");
            pacer.Reset();
        }
    }
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used) {
            SaveState::Free(&saveStates[i]);
        }
    }
    // Anything still holding records after the free loop is leaked: Free
    // clears every slot it touches. Reclaim without engine calls, since the
    // battle those keys point into is being torn down right now.
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        SaveState::Reclaim(&saveStates[i], "battle_close_sweep", i);
    }
    StressCloseBattle();
    if (!summaryEmitted) {
        EmitRollbackDiagSummary("battle_close_deferred");
    }
    (_this->*rSystem::publicMethods.CloseBattle)();
    ResetNativeResultMatch();

    // If the room was lost mid-fight (degraded mode), the fight is now
    // over: exit to a safe disconnected state instead of a fake lobby.
    sf4e::NetplayFacade::FinalizeControlPlaneLossAfterBattle();
}

void fSystem::OnBattleFlow_BattleStart(System* s) {
    if (nNextBattleStartFlowTarget > -1) {
        rSystem::staticMethods.SetBattleFlow(s, nNextBattleStartFlowTarget);
        nNextBattleStartFlowTarget = -1;
        return;
    }

    return rSystem::staticMethods.OnBattleFlow_BattleStart(s);
}

void fSystem::SysMain_HandleTrainingModeFeatures() {
    rSystem* _this = (rSystem*)this;
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    CharaUnit* charaUnit = (CharaUnit*)(_this->*GetUnitByIndex)(rSystem::U_CHARA);

    if (mementoLoadRequest.lo != -1 && mementoLoadRequest.hi != -1) {
        fSystem::RestoreAllFromInternalMementos(_this, &mementoLoadRequest);
        mementoLoadRequest.lo = -1;
        mementoLoadRequest.hi = -1;
    }

    if (mementoSaveRequest.lo != -1 && mementoSaveRequest.hi != -1) {
        fSystem::RecordAllToInternalMementos(_this, &mementoSaveRequest);

        mementoSaveRequest.lo = -1;
        mementoSaveRequest.hi = -1;
    }

    // The developer extended save/load borrows GGPO ring slot 0. While a
    // session owns the pool that slot may be GGPO's, so the request is
    // dropped rather than stealing the slot out from under the ring.
    if (extendedLoadRequest) {
        if (!ggpo && saveStates[0].used) {
            fSystem::SaveState::Load(&saveStates[0]);
        }
        extendedLoadRequest = false;
    }

    if (extendedSaveRequest) {
        if (!ggpo) {
            if (saveStates[0].used) {
                fSystem::SaveState::Free(&saveStates[0]);
            }
            fSystem::SaveState::Save(&saveStates[0]);
        }
        extendedSaveRequest = false;
    }

    (_this->*rSystem::publicMethods.SysMain_HandleTrainingModeFeatures)();
}

void fSystem::SysMain_UpdatePauseState() {
    if (!ggpo) {
        (this->*rSystem::publicMethods.SysMain_UpdatePauseState)();
    }
}

namespace {
// Sums the memento work of each unit group across one Record/RestoreAll pass
// and records one sample per group, so a character whose effects are costly
// shows up as a larger effect/VFX share. Reads no clock when diagnostics are
// off.
class MementoUnitTimer {
public:
    enum Group { CHARA = 0, EFFECT, VFX, OTHER, GROUP_COUNT };
    explicit MementoUnitTimer(const int* ops) : ops_(ops), timed_(diag::Enabled()) {}
    ~MementoUnitTimer() {
        if (!timed_) return;
        for (int group = 0; group < GROUP_COUNT; group++) {
            diag::G().RecordOp(ops_[group], ms_[group]);
        }
    }
    template <class Call> void Time(Group group, Call&& call) {
        if (!timed_) {
            call();
            return;
        }
        const double t0 = diag::NowMs();
        call();
        ms_[group] += diag::NowMs() - t0;
    }
private:
    MementoUnitTimer(const MementoUnitTimer&) = delete;
    MementoUnitTimer& operator=(const MementoUnitTimer&) = delete;
    const int* ops_;
    bool timed_;
    double ms_[GROUP_COUNT] = {};
};

const int kRecordOps[MementoUnitTimer::GROUP_COUNT] = {
    diag::OP_RECORD_CHARA, diag::OP_RECORD_EFFECT, diag::OP_RECORD_VFX, diag::OP_RECORD_OTHER
};
const int kRestoreOps[MementoUnitTimer::GROUP_COUNT] = {
    diag::OP_RESTORE_CHARA, diag::OP_RESTORE_EFFECT, diag::OP_RESTORE_VFX, diag::OP_RESTORE_OTHER
};
}

void fSystem::RestoreAllFromInternalMementos(rSystem* system, rKey::MementoID * id) {
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    CharaUnit* charaUnit = (CharaUnit*)(system->*GetUnitByIndex)(rSystem::U_CHARA);
    MementoUnitTimer timer(kRestoreOps);

    timer.Time(MementoUnitTimer::OTHER, [&] {
        (system->*rSystem::publicMethods.RestoreFromInternalMementoKey)(id);
    });
    timer.Time(MementoUnitTimer::CHARA, [&] {
        (charaUnit->*CharaUnit::publicMethods.RestoreFromInternalMementoKey)(id);
    });
    timer.Time(MementoUnitTimer::EFFECT, [&] {
        (
            ((EffectUnit*)(system->*GetUnitByIndex)(rSystem::U_EFFECT))->*
            EffectUnit::publicMethods.RestoreFromInternalMementoKey
            )(id);
    });
    timer.Time(MementoUnitTimer::VFX, [&] {
        (
            ((VfxUnit*)(system->*GetUnitByIndex)(rSystem::U_VFX))->*
            VfxUnit::publicMethods.RestoreFromInternalMementoKey
            )(id);
    });

    timer.Time(MementoUnitTimer::OTHER, [&] {
        (
            ((CommandUnit*)(system->*GetUnitByIndex)(rSystem::U_COMMAND))->*
            CommandUnit::publicMethods.RestoreFromInternalMementoKey
            )(id);

        (
            ((HudUnit*)(system->*GetUnitByIndex)(rSystem::U_HUD))->*
            HudUnit::publicMethods.RestoreFromInternalMementoKey
            )(id);

        (
            ((CameraUnit*)(system->*GetUnitByIndex)(rSystem::U_CAMERA))->*
            CameraUnit::publicMethods.RestoreFromInternalMementoKey
            )(id);

        (
            TrainingManager::staticMethods.GetSingleton()->*
            TrainingManager::publicMethods.RestoreFromInternalMementoKey
            )(id);
    });

    timer.Time(MementoUnitTimer::CHARA, [&] {
        CharaActor::staticMethods.ResetAfterMemento((charaUnit->*CharaUnit::publicMethods.GetActorByIndex)(0));
        CharaActor::staticMethods.ResetAfterMemento((charaUnit->*CharaUnit::publicMethods.GetActorByIndex)(1));
    });

    // Intentionally omit the reset of the Network unit. All in-game inputs
    // are passed into and read back out of the network unit, regardless
    // of whether or not the match is local or network. The network unit's
    // reset is used to zero the inputs of the first frame after a memento
    // is loaded in training mode, for no real practical reason.
}

void fSystem::RecordAllToInternalMementos(rSystem* system, GameMementoKey::MementoID* id) {
    // This method exists entirely to work around the check that actors are
    // movable before the training mode mementos are saveable. This could be
    // replaced just by no-oping the `jz` instruction at 0x5d7fa0, but this
    // is probably more legible.
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    MementoUnitTimer timer(kRecordOps);
    timer.Time(MementoUnitTimer::OTHER, [&] {
        (system->*rSystem::publicMethods.RecordToInternalMementoKey)(id);
    });

    timer.Time(MementoUnitTimer::CHARA, [&] {
        (
            ((CharaUnit*)(system->*GetUnitByIndex)(rSystem::U_CHARA))->*
            CharaUnit::publicMethods.RecordToInternalMementoKey
            )(id);
    });

    timer.Time(MementoUnitTimer::EFFECT, [&] {
        (
            ((EffectUnit*)(system->*GetUnitByIndex)(rSystem::U_EFFECT))->*
            EffectUnit::publicMethods.RecordToInternalMementoKey
            )(id);
    });

    timer.Time(MementoUnitTimer::VFX, [&] {
        (
            ((VfxUnit*)(system->*GetUnitByIndex)(rSystem::U_VFX))->*
            VfxUnit::publicMethods.RecordToInternalMementoKey
            )(id);
    });

    timer.Time(MementoUnitTimer::OTHER, [&] {
        (
            ((CommandUnit*)(system->*GetUnitByIndex)(rSystem::U_COMMAND))->*
            CommandUnit::publicMethods.RecordToInternalMementoKey
            )(id);

        (
            ((HudUnit*)(system->*GetUnitByIndex)(rSystem::U_HUD))->*
            HudUnit::publicMethods.RecordToInternalMementoKey
            )(id);

        (
            ((CameraUnit*)(system->*GetUnitByIndex)(rSystem::U_CAMERA))->*
            CameraUnit::publicMethods.RecordToInternalMementoKey
            )(id);

        (
            TrainingManager::staticMethods.GetSingleton()->*
            TrainingManager::publicMethods.RecordToInternalMementoKey
            )(id);
    });
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
    if (!ggpo) {
        return;
    }
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
    simGate.OnSessionClosed();
    s_abortLatch.Reset();
    s_disconnectTimeoutMs = 0;
    // Offline play reads bUpdateAllowed directly. A netplay abort or failure
    // closes the gate; without a session that gate must reopen, otherwise the
    // next offline Versus or Training battle never advances a frame.
    bUpdateAllowed = !simGate.manualPause;
    sf4e::NetplayFacade::ClearMatchNotice();
    EmitRollbackDiagSummary(diagnosticsLabel);
    pacer.Reset();
}

void fSystem::AbortGgpoMatch(const char* reason) {
    if (s_abortLatch.Request(reason)) {
        // Inside a GGPO callback: mark the session fatal so the remaining
        // callbacks of this burst do no engine work, remember the reason, and
        // let the outer tick close the session once GGPO has unwound.
        simGate.OnFatal();
        bUpdateAllowed = false;
        spdlog::error("GGPO match abort deferred from callback (depth {}): {}", s_abortLatch.depth, reason ? reason : "");
        return;
    }
    if (reason && reason[0]) {
        spdlog::error("GGPO match abort: {}", reason);
        sf4e::NetplayFacade::PushAlert(reason, sf4e::NoticeSeverity::Error);
    }
    LogSaveSlotOccupancy("abort_entry");
    simGate.OnFatal();
    bUpdateAllowed = false;
    RetireGgpoSession("abort");
    sf4e::NetplayFacade::ClearBattleState();
    rSystem* system = rSystem::staticMethods.GetSingleton();
    if (system) {
        *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
    }
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
    bUpdateAllowed = !simGate.manualPause;
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
    bUpdateAllowed = !simGate.manualPause;
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
    matchTelemetry.Sample(now, GetRemoteNetworkStats(stats) ? stats.network.ping : -1);
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

// A stalled tick already repays time, and the advantage pair is unreliable
// during and after it, so the pacer holds its estimate (OnPredictionStall).
void fSystem::PollTimesync() {
    if (!ggpo || !continuousTimesync || !pacer.enabled) return;
    if (simGate.predictionStalled) {
        pacer.OnPredictionStall();
        return;
    }
    GGPONetworkStats stats{};
    if (MayAdvanceDeterministicFrame() && GetRemoteNetworkStats(stats)) {
        pacer.OnRiftSample(stats.timesync.local_frames_behind, stats.timesync.remote_frames_behind);
    }
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
        CaptureSnapshot(system);
        CaptureHashCheckpoint(system);
    }

    return true;
}

bool fSystem::ggpo_load_game_state_callback(unsigned char* buffer, int len)
{
    GgpoCallbackScope _callbackScope;
    if (simGate.fatalError) {
        return true;
    }
    rollbackHud.Begin(GetTickCount64());
    SaveState* state = (SaveState*)buffer;
    SaveState::Load(state);
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

    // Find an empty position in our array, and store if we can
    // find one.
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used) {
            continue;
        }

        SaveState::Save(&saveStates[i]);
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
        if (IsSpectatorHandle(info->u.connection_interrupted.player)) {
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
            sf4e::NetplayFacade::PushAlert(sf4e::loc::T("runtime.spectator_disconnected"), sf4e::NoticeSeverity::Info);
            break;
        }
        if (system) {
            *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
        }
        simGate.OnConnectionResumed(); // close any open warning episode
        simGate.OnBattleClosing();     // the gate must not report RUNNING for a dead peer
        s_disconnectTimeoutMs = 0;
        sf4e::NetplayFacade::PushAlert(
            sf4e::loc::T(localPlayerHandle == GGPO_INVALID_HANDLE ? "runtime.match_connection_lost" : "runtime.opponent_disconnected"),
            sf4e::NoticeSeverity::Error
        );
        break;
    case GGPO_EVENTCODE_TIMESYNC:
        if (diag::Enabled()) {
            diag::G().OnTimesyncEvent(info->u.timesync.frames_ahead);
        }
        // Phase 4: no blocking here. The recommendation (a fresh clamped
        // estimate of frames ahead — see PacingController) is recorded and
        // repaid in small slices in the outer tick, outside this callback.
        // The continuous path samples the same rift every tick; applying the
        // coarse lump too would correct it twice.
        if (!continuousTimesync) {
            pacer.OnRecommendation(info->u.timesync.frames_ahead);
        }
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
