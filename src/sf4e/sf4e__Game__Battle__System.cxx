#include "sf4e__Game__Battle__System__Internal.hxx"
#include "../common/BattlePause.hxx"
#include "../platform/ReplayFiles.hxx"
#include "sf4e__ReplayPlayback.hxx"
#include "../common/SpectatorCatchUp.hxx"
#include "sf4e__MementoGuards.hxx"

static_assert(sf4e::NativePauseOrHoldFlag == rSystem::SSF_PAUSE_OR_HOLD, "BattlePause.hxx flag differs from the native one");

// Native result state is captured in GGPO saves, including resimulation. The
// history is rewound to each restored state; the emitted latch is deliberately
// match state and survives a rollback so a
// result cannot be submitted twice after re-simulation.
sf4e::native_result::Timeline s_nativeResultTimeline;
static bool s_nativeResultEmitted = false;

void ResetNativeResultMatch() {
    s_nativeResultTimeline.Reset();
    s_nativeResultEmitted = false;
}

// The outcome is settled: this fighter confirmed it natively, or the room
// committed the match end (the only way a spectator learns it). Losing the
// stream or the peer after that ends nothing the player needs to hear about.
bool MatchResultKnown() { return s_nativeResultEmitted || sf4e::NetplayFacade::IsRuntimeMatchEndCommitted(); }

void CaptureNativeMatchResult(rSystem* system, int stateFrame) {
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
    const auto sample = s_nativeResultTimeline.ConfirmedSample(confirmed);
    const auto result = sample.result;
    if (result == sf4e::native_result::Result::None) return;

    s_nativeResultEmitted = true;
    spdlog::info("Match result: confirmed native outcome={} input_frame={} save_frame={}", static_cast<int>(result), confirmed, sample.frame);
    const auto captureFrame = static_cast<std::uint64_t>(sample.frame);
    const auto confirmedFrame = static_cast<std::uint64_t>(confirmed < 0 ? 0 : confirmed);
    switch (result) {
    case sf4e::native_result::Result::P1Win:
        sf4e::NetplayFacade::NotifyRuntimeMatchResult(sf4e::room::MatchResult::P1Win, captureFrame, confirmedFrame);
        break;
    case sf4e::native_result::Result::P2Win:
        sf4e::NetplayFacade::NotifyRuntimeMatchResult(sf4e::room::MatchResult::P2Win, captureFrame, confirmedFrame);
        break;
    case sf4e::native_result::Result::Draw:
        sf4e::NetplayFacade::NotifyRuntimeMatchResult(sf4e::room::MatchResult::Draw, captureFrame, confirmedFrame);
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

int fSystem::nExtraFramesToSimulate = 0;
int fSystem::nNextBattleStartFlowTarget = -1;
int fSystem::nRandomizeLocalInputsEveryXFramesInGGPO = 0;

GGPOPlayerHandle fSystem::localPlayerHandle = GGPO_INVALID_HANDLE;
int fSystem::lastGgpoSaveFrame = -1;
GGPOSession* fSystem::ggpo = nullptr;
sf4e::gate::GgpoGateModel fSystem::simGate = { sf4e::gate::PHASE_NO_SESSION };
// Zeroed (so disabled) until ResetPacerForSession initializes it.
sf4e::pacing::PacingController fSystem::pacer;

bool fSystem::MayAdvanceDeterministicFrame() {
    // bUpdateAllowed is the developer's manual gate; the model owns the
    // session lifecycle and terminal state. Connection warnings and
    // prediction stalls intentionally remain absent.
    return simGate.MayAdvance(ggpo != nullptr, bUpdateAllowed);
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

// The notice view keeps one player per simulated fighter and the memento room
// for two. A frame simulating more cannot be recorded or restored whole.
static int NoticePlayers(rSystem* system, bool& failure) {
    const int simulated = (system->*rSystem::publicMethods.GetNumCharasToSimulateThisFrame)();
    constexpr int slots = sizeof(fSystem::AdditionalMemento::playerNotices) / sizeof(fSystem::AdditionalMemento::playerNotices[0]);
    if (simulated > slots) failure = true;
    return simulated < slots ? simulated : slots;
}

static bool s_systemMementoLayoutValid = true;

int fSystem::GetMementoSize() {
    const int native = (this->*rSystem::mementoableMethods.GetMementoSize)();
    s_systemMementoLayoutValid = native == static_cast<int>(sizeof(Memento));
    if (!s_systemMementoLayoutValid) sf4e::Game::MementoFailure::record = true;
    // The additional memento is placed right after sizeof(Memento) bytes. A
    // native memento of another size would put it past the allocation (smaller)
    // or over the engine's own data (larger). Reject that layout before record.
    static bool noted = false;
    static bool notedMismatch = false;
    if (!s_systemMementoLayoutValid && !notedMismatch) {
        notedMismatch = true;
        spdlog::error("System memento: the engine's is {:#x} bytes, but the additional memento is placed after {:#x}; record refused",
            native, sizeof(Memento));
    } else if (s_systemMementoLayoutValid && !noted) {
        noted = true;
        spdlog::info("System memento: {:#x} bytes plus {:#x} additional", native, sizeof(AdditionalMemento));
    }
    return native + sizeof(AdditionalMemento);
}

int fSystem::RecordToMemento(Memento* m, GameMementoKey::MementoID* id) {
    if (!s_systemMementoLayoutValid) {
        sf4e::Game::MementoFailure::record = true;
        return 0;
    }
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
    const int noticeCount = NoticePlayers(_this, sf4e::Game::MementoFailure::record);
    for (int playerIdx = 0; playerIdx < noticeCount; playerIdx++) {
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
    if (!s_systemMementoLayoutValid) {
        sf4e::Game::MementoFailure::restore = true;
        return 0;
    }
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
    const int noticeCount = NoticePlayers(_this, sf4e::Game::MementoFailure::restore);
    for (int playerIdx = 0; playerIdx < noticeCount; playerIdx++) {
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
                *rHud::Training::Unit::GetHudTrainingUpdateTask(*HudUnit::GetTraining(hud)) = cursor;
            }
        }
    }

    const int result = (this->*rSystem::mementoableMethods.RestoreFromMemento)(m, id);
    return result;
}


// One deterministic frame from GGPO's inputs: synchronize, run the engine
// update, advance. False when no frame was played: a transient refusal, or an
// abort that retired the session.
static bool PlayGgpoFrame(rSystem* system) {
    fPadSystem::Inputs ggpoInputs[2] = { {0, 0}, {0, 0} };
    int disconnect_flags = 0;
    GGPOErrorCode result;
    {
        diag::ScopedTimer _t(diag::OP_SYNC_INPUT);
        result = ggpo_synchronize_input(fSystem::ggpo, (void*)ggpoInputs, sizeof(fPadSystem::Inputs) * 2, &disconnect_flags);
    }
    if (diag::Enabled()) {
        diag::G().RecordGgpoResult(diag::CALL_SYNC_INPUT, (int)result);
    }
    if (fSystem::DrainPendingAbort()) {
        return false;
    }
    switch (sf4e::gate::ClassifyGgpoResult((int)result)) {
    case sf4e::gate::POLICY_CONTINUE:
        break;
    case sf4e::gate::POLICY_FATAL:
        if (fSystem::localPlayerHandle == GGPO_INVALID_HANDLE) {
            EndSpectatorStream((int)result);
            return false;
        }
        spdlog::error("GGPO: synchronize_input returned irrecoverable {}", (int)result);
        fSystem::AbortGgpoMatch(sf4e::loc::T("runtime.netplay_sync_failed"));
        return false;
    default:
        // NOT_SYNCHRONIZED during startup/resync, or another
        // transient refusal: skip this frame without simulating.
        if (diag::Enabled()) {
            diag::G().RecordSkip(diag::SKIP_SYNC_INPUT_ERROR, diag::NowMs());
        }
        return false;
    }
    NoteDisconnectFlags(disconnect_flags);
    // The game is never shown the reserved bits (TrainingRuntime.hxx).
    ggpoInputs[0].rawOn = sf4e::training::ClearReservedInputBits(ggpoInputs[0].rawOn);
    ggpoInputs[1].rawOn = sf4e::training::ClearReservedInputBits(ggpoInputs[1].rawOn);
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
            (system->*rSystem::publicMethods.BattleUpdate)();
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
        err = ggpo_advance_frame(fSystem::ggpo);
    }
    if (diag::Enabled()) {
        diag::G().RecordGgpoResult(diag::CALL_ADVANCE_FRAME, (int)err);
    }
    if (fSystem::DrainPendingAbort()) {
        return false;
    }
    if (!GGPO_SUCCEEDED(err)) {
        spdlog::error("GGPO: advance_frame returned {}", (int)err);
        fSystem::AbortGgpoMatch(sf4e::loc::T("runtime.netplay_sync_failed"));
        return false;
    }
    fSystem::simGate.OnFrameAccepted();
    if (diag::Enabled()) {
        diag::G().OnFrameAdvanced(diag::NowMs());
    }
    if (fSoundPlayerManager::bUsePureSounds) {
        fSoundPlayerManager::SyncState();
    }
    fSystem::CaptureSnapshot(system);
    fSystem::CaptureHashCheckpoint(system);
    fSystem::CaptureMeterSample(system, true);
    PublishConfirmedNativeMatchResult();
    return true;
}

// The host never waits for a spectator, so a spectator that fell behind its
// stream plays a few extra frames per update until it is back within its
// reserve (SpectatorCatchUp). Called after the update's own frame.
static void CatchUpSpectator(rSystem* system) {
    int backlog = 0;
    if (!GGPO_SUCCEEDED(ggpo_get_spectator_backlog(fSystem::ggpo, &backlog))) return;
    const int extra = sf4e::SpectatorCatchUp::ExtraFrames(backlog);
    int played = 0;
    while (played < extra && fSystem::ggpo && fSystem::MayAdvanceDeterministicFrame() &&
        *rSystem::staticVars.CurrentBattleFlow != rSystem::BF__IDLE && PlayGgpoFrame(system)) {
        ++played;
    }
    NoteSpectatorBacklog(backlog, played);
}

void fSystem::BattleUpdate() {
    sf4e::memento::DrainAbort();
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
                        inputs.rawOn = sf4e::training::ClearReservedInputBits(inputs.rawOn);
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

        if (!PlayGgpoFrame(_this)) {
            return;
        }
        if (localPlayerHandle == GGPO_INVALID_HANDLE) {
            CatchUpSpectator(_this);
        }
    }
    else if (simGate.NativeExitRequired()) {
        LeaveOrphanedNetplayBattle(_this);
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
        // A replay shown with the frame meter: the frame just played goes to
        // the meter as a watched match's does, with no input to confirm. A
        // frame the replay controls (or the game's half speed) held is not
        // one: the meter would take the repeated frame as a gap and start over.
        if (sf4e::replayplayback::FeedMeter(_this)) sf4e::training::ObserveMatch(_this, 0, -1);
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

// Rollback restores a particle's fields with a raw copy (0x5BF760), its name
// string included. A name longer than the 15 characters kept inline lives on
// the heap, and such a copy could bring back a freed buffer. Says once whether
// a netplay battle ever has one.
static void NoteHeapParticleNames(rSystem* system) {
    namespace rVfx = Dimps::Game::Battle::Vfx;
    static bool noted = false;
    if (noted || !system) return;
    auto* unit = (VfxUnit*)(system->*rSystem::publicMethods.GetUnitByIndex)(rSystem::U_VFX);
    if (!unit) return;
    auto* particles = (rVfx::ParticleContainer*)(unit->*VfxUnit::publicMethods.GetContainerByType)(VfxUnit::CT_PARTICLE);
    if (!particles) return;
    unsigned heapNames = 0;
    std::size_t longest = 0;
    for (unsigned int i = 0; i < rVfx::ParticleContainer::DEFAULT_PARTICLE_COUNT; i++) {
        rVfx::Particle* particle = (particles->*rVfx::ParticleContainer::publicMethods.GetParticleFromHandle)(
            rVfx::ParticleContainer::GenerateFakeHandle(i));
        if (!particle) continue;
        const std::size_t length = rVfx::Particle::GetNameTmp(particle)->size();
        if (length > 15) { ++heapNames; longest = (std::max)(longest, length); }
    }
    if (!heapNames) return;
    noted = true;
    spdlog::warn("VFX: {} live particles have names on the heap (longest {}); rollback copies them raw", heapNames, longest);
}

void fSystem::CloseBattle() {
    rSystem* _this = (rSystem*)this;
    // The engine is closing this battle, so a session retired from here on
    // (now, or later by the spectator drain) leaves no orphan behind.
    const bool netplayBattle = simGate.OnNativeBattleClosed();
    sf4e::battlePause.CloseBattle();
    sf4e::training::CloseBattle();
    sf4e::replayplayback::CloseBattle();
    bool summaryEmitted = false;
    LogSaveSlotOccupancy("battle_close_entry");
    sf4e::crash::NoteMatchBoundary("battle_close");
    if (ggpo) NoteHeapParticleNames(_this);
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
        if (!sf4e::NetplayFacade::DrainingSpectators()) {
            RetireGgpoSession("battle_close");
            summaryEmitted = true;
        }
        else {
            if (diag::Enabled()) {
                diag::G().OnSessionEnded(diag::NowMs());
            }
            LogPacerSummary("battle_close_deferred");
            ResetPacing();
        }
    } else if (netplayBattle) {
        // The session was retired while the battle was alive (a lost spectator
        // stream, a failed start, a played-out view), so no match-ended notice
        // comes from above. The runtime still has to learn the battle is gone.
        spdlog::info("Match: netplay battle closed after its session was retired");
        sf4e::NetplayFacade::NotifyRuntimeBattleClosedWithoutSession();
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
    sf4e::platform::replays::NoteMatchEnd();
    (_this->*rSystem::publicMethods.CloseBattle)();
    sf4e::memento::LogCounters("battle_close_exit");
    sf4e::memento::ResetCounters();
    ResetNativeResultMatch();

    // If the room was lost mid-fight (degraded mode), the fight is now
    // over: exit to a safe disconnected state instead of a fake lobby.
    sf4e::NetplayFacade::FinalizeControlPlaneLossAfterBattle();
}

void fSystem::OnBattleFlow_BattleStart(System* s) {
    sf4e::battlePause.StartBattle();
    sf4e::replayplayback::StartBattle(s);
    StressOpenBattle();
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
        if (!ggpo && saveStates[0].used && !fSystem::SaveState::Load(&saveStates[0])) {
            spdlog::error("Developer extended load: the state did not fully restore; leaving the battle");
            *rSystem::GetReadyState(_this) = rSystem::RS_ISLEAVING;
        }
        extendedLoadRequest = false;
    }

    if (extendedSaveRequest) {
        if (!ggpo) {
            if (saveStates[0].used) {
                fSystem::SaveState::Free(&saveStates[0]);
            }
            if (!fSystem::SaveState::Save(&saveStates[0])) {
                spdlog::error("Developer extended save: this state cannot be saved");
            }
        }
        extendedSaveRequest = false;
    }

    (_this->*rSystem::publicMethods.SysMain_HandleTrainingModeFeatures)();
}

void fSystem::SysMain_UpdatePauseState() {
    if (simGate.LocalControllerOwnsBattle(ggpo != nullptr)) {
        (this->*rSystem::publicMethods.SysMain_UpdatePauseState)();
        sf4e::battlePause.Publish(sf4e::NativePauseMenuOpen(*rSystem::GetSimulationFlags(this), *rSystem::GetPausingPlayer(this)));
    } else {
        sf4e::battlePause.Publish(false);
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
