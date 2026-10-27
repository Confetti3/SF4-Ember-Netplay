#include "sf4e__Game__Battle__System__Internal.hxx"
#include "../common/BattlePause.hxx"
#include "../platform/ReplayFiles.hxx"
#include "sf4e__ReplayPlayback.hxx"
#include "sf4e__InputLag.hxx"
#include "../common/SpectatorCatchUp.hxx"
#include "sf4e__MementoGuards.hxx"
#include "../common/MementoGuards.hxx"
#include "sf4e__CustomContent.hxx"

static_assert(sf4e::NativePauseOrHoldFlag == rSystem::SSF_PAUSE_OR_HOLD, "BattlePause.hxx flag differs from the native one");

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
fSystem::SessionRole fSystem::sessionRole = fSystem::SessionRole::None;
bool fSystem::drainingSpectators = false;
sf4e::statehash::GgpoTimeline fSystem::ggpoTimeline;
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


// It is important that this calls the _original_, undetoured method: the
// detoured fSystem::BattleUpdate is restricted to the same update-halting
// that a frame of the present is, and a re-simulated frame must not be.
void fSystem::StepEngine(rSystem* system, const fPadSystem::Inputs inputs[2], bool syncSoundBeforeStep) {
    // Playback mode ends with the engine update, on every exit: the sample,
    // ggpo_advance_frame's save and any nested GGPO work must not read the
    // stale playback inputs, nor may it leak into later engine work.
    PlaybackFrameScopeGuard _playbackGuard;
    fPadSystem::playbackFrame = 0;
    fPadSystem::playbackData[0][0] = inputs[0];
    fPadSystem::playbackData[0][1] = inputs[1];
    if (syncSoundBeforeStep && fSoundPlayerManager::bUsePureSounds) {
        fSoundPlayerManager::SyncState();
    }
    diag::ScopedTimer _t(diag::OP_ENGINE_BATTLE_UPDATE);
    (system->*rSystem::publicMethods.BattleUpdate)();
}

GGPOErrorCode fSystem::ReadGgpoInputs(fPadSystem::Inputs inputs[2], int* disconnectFlags, FramePass pass) {
    GGPOErrorCode result;
    if (pass == FramePass::Live) {
        diag::ScopedTimer _t(diag::OP_SYNC_INPUT);
        result = ggpo_synchronize_input(ggpo, (void*)inputs, sizeof(fPadSystem::Inputs) * 2, disconnectFlags);
    }
    else {
        result = ggpo_synchronize_input(ggpo, (void*)inputs, sizeof(fPadSystem::Inputs) * 2, disconnectFlags);
    }
    if (diag::Enabled()) {
        diag::G().RecordGgpoResult(diag::CALL_SYNC_INPUT, (int)result);
    }
    return result;
}

GGPOErrorCode fSystem::SimulateGgpoFrame(rSystem* system, fPadSystem::Inputs inputs[2], FramePass pass) {
    // The game is never shown the reserved bits (TrainingRuntime.hxx), in a
    // frame of the present or its re-simulation alike.
    inputs[0].rawOn = sf4e::training::ClearReservedInputBits(inputs[0].rawOn);
    inputs[1].rawOn = sf4e::training::ClearReservedInputBits(inputs[1].rawOn);
    const bool live = pass == FramePass::Live;
    const GGPOErrorCode err = sf4e::statehash::GgpoTimeline::Simulate(
        [&] { StepEngine(system, inputs, live); },
        [&] { CaptureSimulatedFrame(system, inputs); },
        [&]() -> GGPOErrorCode {
            // The live advance is timed on its own; inside a rollback it is
            // already part of the enclosing advance.
            if (!live) {
                return ggpo_advance_frame(ggpo);
            }
            diag::ScopedTimer _t(diag::OP_ADVANCE_FRAME_API);
            return ggpo_advance_frame(ggpo);
        });
    if (diag::Enabled()) {
        diag::G().RecordGgpoResult(diag::CALL_ADVANCE_FRAME, (int)err);
    }
    return err;
}

// One deterministic frame from GGPO's inputs: synchronize, run the engine
// update, advance. False when no frame was played: a transient refusal, or an
// abort that retired the session.
static bool PlayGgpoFrame(rSystem* system) {
    fPadSystem::Inputs ggpoInputs[2] = { {0, 0}, {0, 0} };
    int disconnect_flags = 0;
    const GGPOErrorCode result = fSystem::ReadGgpoInputs(ggpoInputs, &disconnect_flags, fSystem::FramePass::Live);
    if (fSystem::DrainPendingAbort()) {
        return false;
    }
    switch (sf4e::gate::ClassifyGgpoResult((int)result)) {
    case sf4e::gate::POLICY_CONTINUE:
        break;
    case sf4e::gate::POLICY_FATAL:
        if (!fSystem::LocalFighter()) {
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
    const GGPOErrorCode err = fSystem::SimulateGgpoFrame(system, ggpoInputs, fSystem::FramePass::Live);
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
        PlayGgpoFrame(system)) {
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
    sf4e::InputLag::BeforeBattleFrame(sf4e::NetplayFacade::GetRuntimeSnapshotShared()->preferences.lowerInputLag);

    if (!MayAdvanceDeterministicFrame()) {
        if (ggpo && diag::Enabled()) {
            diag::G().RecordSkip(diag::SKIP_UPDATE_GATE, diag::NowMs());
        }
        return;
    }

    // Bootstrap idle is allowed before the first deterministic frame only.
    // Round-transition idle must use the same inputs/frame accounting as
    // ggpo_advance_frame_callback, which also replays those engine updates.
    if (simGate.UsesGgpoTimeline(ggpo != nullptr, *rSystem::staticVars.CurrentBattleFlow == BF__IDLE)) {
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
        if (LocalFighter()) {
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
        if (!LocalFighter()) {
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
        // is also taken with a live session during initial bootstrap, so it
        // must never run alongside GGPO's own pool.
        if (ggpo || !StressStep(_this)) {
            (_this->*rSystem::publicMethods.BattleUpdate)();
        }
        sf4e::training::AfterUpdate(_this);
        // A replay's playback observes the recorder as this update left it
        // (the strip, the lanes and an export's progress read it). Shown with
        // the frame meter, the frame just played goes to the meter as a
        // watched match's does, with no input to confirm. A
        // frame the replay controls (or the game's half speed) held is not
        // one: the meter would take the repeated frame as a gap and start over.
        if (sf4e::replayplayback::AfterUpdate(_this)) {
            unsigned inputs[2] = {0, 0};
            sf4e::training::ObserveMatch(_this, 0, -1, sf4e::replayplayback::PlayedInputs(inputs) ? inputs : nullptr);
        }
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
    sf4e::InputLag::CloseBattle();
    sf4e::custom::EndBattle();
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
            ggpoTimeline.Held()
        );
        simGate.OnBattleClosing();
        // Decide defer *before* close so a prior spectator defer flag cannot
        // leave this session open across rematch.
        sf4e::NetplayFacade::NotifyMatchEnded();
        if (!drainingSpectators) {
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
    // After the native close, which puts back its own saved step setting.
    RestoreBattleStep();
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

