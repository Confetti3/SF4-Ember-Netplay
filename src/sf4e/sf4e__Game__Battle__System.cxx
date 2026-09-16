#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>

#include <windows.h>
#include <detours/detours.h>
#include <ggponet.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__Camera.hxx"
#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__Command.hxx"
#include "../Dimps/Dimps__Game__Battle__Effect.hxx"
#include "../Dimps/Dimps__Game__Battle__Hud.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Game__Battle__Training.hxx"
#include "../training/TrainingRuntime.hxx"
#include "../Dimps/Dimps__Game__Battle__Vfx.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"

#include "../common/sf4e__RollbackDiagnostics.hxx"
#include "../common/sf4e__GgpoAbortLatch.hxx"
#include "../common/RollbackHud.hxx"
static sf4e::RollbackHud rollbackHud;
#include "../common/sf4e__StateHash.hxx"
#include "../common/NativeMatchResult.hxx"
#include "../session/sf4e__SessionProtocol.hxx"

#include "sf4e.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__Hud.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__Platform.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__Overlay.hxx"

using Dimps::Platform::WithReleaser;

namespace rHud = Dimps::Game::Battle::Hud;
using CameraUnit = Dimps::Game::Battle::Camera::Unit;
using CharaActor = Dimps::Game::Battle::Chara::Actor;
using CharaUnit = Dimps::Game::Battle::Chara::Unit;
using CommandUnit = Dimps::Game::Battle::Command::Unit;
using EffectUnit = Dimps::Game::Battle::Effect::Unit;
using GameManager = Dimps::Game::Battle::GameManager;
using HudUnit = Dimps::Game::Battle::Hud::Unit;
using NetworkUnit = Dimps::Game::Battle::Network::Unit;
using rSoundPlayerManager = Dimps::Game::Battle::Sound::SoundPlayerManager;
using rSystem = Dimps::Game::Battle::System;
using PauseUnit = Dimps::Game::Battle::Pause::Unit;
using TrainingManager = Dimps::Game::Battle::Training::Manager;
using VfxUnit = Dimps::Game::Battle::Vfx::Unit;
using rKey = Dimps::Game::GameMementoKey;
using FixedPoint = Dimps::Math::FixedPoint;
using fKey = sf4e::Game::GameMementoKey;
using rPadSystem = Dimps::Pad::System;
using fPadSystem = sf4e::Pad::System;
using StateSnapshot = sf4e::SessionProtocol::StateSnapshot;
namespace diag = sf4e::diag;

namespace fHud = sf4e::Game::Battle::Hud;
using fSoundPlayerManager = sf4e::Game::Battle::Sound::SoundPlayerManager;
using fSystem = sf4e::Game::Battle::System;
using fVsBattle = sf4e::GameEvents::VsBattle;
using rSystem = Dimps::Game::Battle::System;

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
static sf4e::native_result::Timeline s_nativeResultTimeline;
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

// Defined with SaveState::Free.
static void LogSaveStateFreePolicy();
static const char* SaveStateFreePathName();

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

// SaveState operations mutate live engine objects (memento record/restore,
// key clearing) and are only valid on the game main thread — the thread that
// runs BattleUpdate, Steam_PostUpdate, and every GGPO callback (see
// docs/GGPO_LIFECYCLE.md). Debug builds assert this; engine-memento work
// must never move to a background thread.
static DWORD s_saveStateThreadId = 0;
static void AssertSaveStateThreadAffinity() {
    DWORD tid = GetCurrentThreadId();
    if (s_saveStateThreadId == 0) {
        s_saveStateThreadId = tid;
        return;
    }
    if (tid != s_saveStateThreadId) {
        // Release builds previously latched the id and said nothing, so a
        // violation was invisible in exactly the builds players run. There is
        // no safe recovery (the engine memento calls are already in flight),
        // but the log line makes it diagnosable instead of silent.
        static bool s_warnedThreadAffinity = false;
        if (!s_warnedThreadAffinity) {
            s_warnedThreadAffinity = true;
            spdlog::error(
                "SaveState: op ran on thread {} but pool is owned by thread {}",
                tid,
                s_saveStateThreadId
            );
        }
        assert(false && "SaveState ops must stay on the game main thread");
    }
}

// Restores pad playback mode on every exit path. GGPO input playback must
// never leak past the simulation step that installed it.
struct PlaybackFrameScopeGuard {
    ~PlaybackFrameScopeGuard() { fPadSystem::playbackFrame = -1; }
};

// Emits the complete diagnostics summary through the normal log. Called at
// match end / abort, before battle state is torn down.
static void EmitRollbackDiagSummary(const char* label) {
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
// maxRecommendationFrames, maxStepMs, minWaitMs, enabled; state/stats zeroed.
sf4e::pacing::PacingController fSystem::pacer = { 9.0, 3.0, 1.0, true };

// Applies development overrides for the pacing caps and resets the
// controller for a new session. Called from StartGGPO/StartSpectating.
static void ResetPacerForSession() {
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
    fSystem::pacer.Reset();
    fSystem::pacer.ResetStats();
}

static void LogPacerSummary(const char* label) {
    const sf4e::pacing::PacingController& p = fSystem::pacer;
    if (p.recommendationsReceived == 0 && p.msAppliedTotal == 0.0) {
        return;
    }
    spdlog::info(
        "Pacing [{}]: enabled={} recs={} framesRec={} acceptedMs={:.1f} "
        "replacedMs={:.1f} disabledDiscardMs={:.1f} resetDiscardMs={:.1f} "
        "waits={} requestedMs={:.1f} actualMs={:.1f} maxRequestedMs={:.2f} "
        "maxActualMs={:.2f} failures={} timeouts={} fallbacks={} "
        "maxOutstandingMs={:.1f} outstandingMs={:.1f}",
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
        p.outstandingMs
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

// ---------------------------------------------------------------------------
// Local rollback stress (development only).
//
// SF4E_ROLLBACK_STRESS=<1..8> makes an offline battle (Versus or Training)
// drive save states the way a GGPO session with that rollback distance does:
//
//   * every frame frees the oldest ring slot and saves before simulating;
//   * every <distance> frames the state from <distance> frames ago is loaded
//     and those frames are re-simulated with their recorded inputs, freeing
//     and saving again on each one, exactly as Sync::AdjustSimulation does.
//
// After each re-simulated frame the semantic hash is compared with the
// original pass and any divergence is logged with its subsystem. Inputs pass
// through the same playback path as netplay, and Training Lab playback is
// honoured, so a recorded sequence can be replayed under rollback on one PC.
// With SF4E_ROLLBACK_DIAGNOSTICS=1 the usual timing summary is logged every
// 600 frames, which makes per-character save/free/restore cost comparable
// without a network. GGPO's own synctest backend is not used: it breaks into
// the debugger on a mismatch and writes a log file for every frame.
// ---------------------------------------------------------------------------
namespace {
struct RollbackStress {
    static constexpr int kRing = NUM_SAVE_STATES;
    int distance = 0; // 0 disables
    bool configured = false;
    bool primed = false;
    int frame = 0;
    int16_t lastEngineFrame = 0;
    fSystem::SaveState states[kRing];
    int stateFrame[kRing] = {};
    fPadSystem::Inputs inputs[kRing][2] = {};
    // hashes[g % kRing] is the state after simulating stress frame g.
    fSystem::SemanticHashes hashes[kRing];
    uint64_t rollbacks = 0;
    uint64_t divergences = 0;
    uint64_t resets = 0;
};

RollbackStress& Stress() {
    static RollbackStress stress;
    return stress;
}

int16_t StressEngineFrame(rSystem* system) {
    return rSystem::GetNumFramesSimulated_FixedPoint(system)->integral;
}

void StressConfigure() {
    auto& stress = Stress();
    if (stress.configured) {
        return;
    }
    stress.configured = true;
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA("SF4E_ROLLBACK_STRESS", value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) {
        return;
    }
    const int distance = atoi(value);
    if (distance < 1 || distance > GGPO_MAX_PREDICTION_FRAMES) {
        spdlog::warn("RollbackStress: SF4E_ROLLBACK_STRESS={} ignored; use 1..{}", value, GGPO_MAX_PREDICTION_FRAMES);
        return;
    }
    stress.distance = distance;
    spdlog::warn("RollbackStress: enabled for offline battles, distance={} frames", distance);
}

void StressReset() {
    auto& stress = Stress();
    for (int i = 0; i < RollbackStress::kRing; i++) {
        if (stress.states[i].used) {
            fSystem::SaveState::Free(&stress.states[i]);
        }
        stress.stateFrame[i] = -1;
    }
    stress.frame = 0;
    stress.primed = false;
}

// Drops any records left over from a battle that ended without reaching
// StressCloseBattle. Their keys point into destroyed objects, so this must
// not call the engine (same rule as fSystem::StartGGPO's sweep).
void StressReclaimAll(const char* reason) {
    auto& stress = Stress();
    for (int i = 0; i < RollbackStress::kRing; i++) {
        fSystem::SaveState::Reclaim(&stress.states[i], reason, i);
        stress.stateFrame[i] = -1;
    }
    stress.frame = 0;
    stress.primed = false;
}

fPadSystem::Inputs StressReadInput(rPadSystem* pad, int side) {
    sf4e::training::Input practice;
    if (sf4e::training::ReadOverride(side, practice)) {
        return { practice.mapped, practice.raw };
    }
    if (sf4e::Overlay::CapturesMenuInput()) {
        return { 0, 0 };
    }
    return {
        (pad->*rPadSystem::publicMethods.GetButtons_MappedOn)(side),
        (pad->*rPadSystem::publicMethods.GetButtons_RawOn)(side),
    };
}

void StressSimulate(rSystem* system, const fPadSystem::Inputs* inputs) {
    PlaybackFrameScopeGuard playbackGuard;
    fPadSystem::playbackFrame = 0;
    fPadSystem::playbackData[0][0] = inputs[0];
    fPadSystem::playbackData[0][1] = inputs[1];
    diag::ScopedTimer _t(diag::OP_ENGINE_BATTLE_UPDATE);
    (system->*rSystem::publicMethods.BattleUpdate)();
}

// GGPO frees the ring slot, then saves, before every simulated frame.
void StressSaveBefore(int stressFrame) {
    auto& stress = Stress();
    const int index = stressFrame % RollbackStress::kRing;
    if (stress.states[index].used) {
        fSystem::SaveState::Free(&stress.states[index]);
    }
    fSystem::SaveState::Save(&stress.states[index]);
    stress.stateFrame[index] = stressFrame;
}

const char* SubsystemState(uint64_t replay, uint64_t original) {
    return replay == original ? "match" : "DIFF";
}

// Runs this outer frame's update under the stress schedule. Returns false when
// stress is disabled so the caller runs the plain update.
bool StressStep(rSystem* system) {
    StressConfigure();
    auto& stress = Stress();
    if (!stress.distance) {
        return false;
    }
    const int16_t before = StressEngineFrame(system);
    if (stress.primed && before != stress.lastEngineFrame) {
        // A training restore or other jump outside this loop: the recorded
        // history no longer describes the live timeline.
        StressReset();
        stress.resets++;
    }
    if (!stress.primed) {
        // A previous battle that never reached StressCloseBattle leaves
        // slots pointing into freed engine objects. Sweep before the first
        // save of this battle.
        StressReclaimAll("stress_prime");
        diag::InitFromEnvironment();
        if (diag::Enabled() && stress.rollbacks == 0) {
            diag::G().ResetForMatch(diag::NowMs());
        }
        stress.primed = true;
    }

    rPadSystem* pad = rPadSystem::staticMethods.GetSingleton();
    const int current = stress.frame;
    const int index = current % RollbackStress::kRing;
    stress.inputs[index][0] = StressReadInput(pad, 0);
    stress.inputs[index][1] = StressReadInput(pad, 1);
    StressSaveBefore(current);
    StressSimulate(system, stress.inputs[index]);
    stress.lastEngineFrame = StressEngineFrame(system);
    if (static_cast<int16_t>(stress.lastEngineFrame - before) != 1) {
        // Paused, or the native flow held the simulation: not a replayable
        // frame. Start the history again from the next one.
        StressReset();
        stress.primed = true;
        return true;
    }
    stress.hashes[index] = fSystem::ComputeSemanticHashes(system);
    stress.frame++;
    if (diag::Enabled()) {
        diag::G().OnFrameAdvanced(diag::NowMs());
    }

    if (stress.frame < stress.distance || stress.frame % stress.distance != 0) {
        return true;
    }
    const int target = stress.frame - stress.distance;
    const int targetIndex = target % RollbackStress::kRing;
    if (!stress.states[targetIndex].used || stress.stateFrame[targetIndex] != target) {
        return true;
    }
    fSystem::SaveState::Load(&stress.states[targetIndex]);
    for (int replayed = target; replayed < stress.frame; replayed++) {
        diag::ScopedTimer _cb(diag::OP_ROLLBACK_CALLBACK);
        if (diag::Enabled()) {
            diag::G().OnRollbackCallback(diag::NowMs());
        }
        const int replayIndex = replayed % RollbackStress::kRing;
        StressSaveBefore(replayed);
        StressSimulate(system, stress.inputs[replayIndex]);
        const auto replay = fSystem::ComputeSemanticHashes(system);
        const auto& original = stress.hashes[replayIndex];
        if (replay.overall != original.overall) {
            stress.divergences++;
            spdlog::error(
                "RollbackStress: replay diverged stress_frame={} engine_frame={} distance={} free_path={} flow={} p1={} p2={} inputs={:08x}/{:08x} {:08x}/{:08x}",
                replayed,
                StressEngineFrame(system),
                stress.distance,
                SaveStateFreePathName(),
                SubsystemState(replay.flow, original.flow),
                SubsystemState(replay.chara[0], original.chara[0]),
                SubsystemState(replay.chara[1], original.chara[1]),
                stress.inputs[replayIndex][0].mappedOn,
                stress.inputs[replayIndex][0].rawOn,
                stress.inputs[replayIndex][1].mappedOn,
                stress.inputs[replayIndex][1].rawOn
            );
            // Continue from the replayed timeline so one divergence is not
            // reported again on every later rollback.
            stress.hashes[replayIndex] = replay;
        }
    }
    stress.rollbacks++;
    stress.lastEngineFrame = StressEngineFrame(system);

    if (stress.rollbacks % (600 / stress.distance) == 0) {
        spdlog::info(
            "RollbackStress: distance={} free_path={} rollbacks={} divergences={} resets={}",
            stress.distance, SaveStateFreePathName(), stress.rollbacks, stress.divergences, stress.resets
        );
        EmitRollbackDiagSummary("rollback_stress");
    }
    return true;
}

void StressCloseBattle() {
    auto& stress = Stress();
    if (!stress.distance) {
        return;
    }
    spdlog::info(
        "RollbackStress: battle closed distance={} free_path={} rollbacks={} divergences={} resets={}",
        stress.distance, SaveStateFreePathName(), stress.rollbacks, stress.divergences, stress.resets
    );
    EmitRollbackDiagSummary("rollback_stress_close");
    StressReset();
    stress.rollbacks = stress.divergences = stress.resets = 0;
}
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
            AbortGgpoMatch("Netplay input failed. The match has ended.");
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
                AbortGgpoMatch("Netplay sync failed. The match has ended.");
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
                AbortGgpoMatch("Netplay sync failed. The match has ended.");
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
        sf4e::NetplayFacade::PushAlert("Invalid match roster. Return to the room and try again.");
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
    // Do not reset GgpoRelay here. The legacy Direct-IP/session-tunnel path
    // creates its virtual peer immediately before calling StartGGPO; resetting
    // it here destroys the transport before GGPO can exchange its handshake.
    // GgpoRelay::Start handles stale state, and battle close/abort own teardown.
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
        AbortGgpoMatch("GGPO could not start — return to lobby and Ready again.");
        return;
    }
    spdlog::info("GGPO: session started localPort={}", port);
    ApplyGgpoDisconnectSettings(ggpo);

    int localPlayerIdx = -1;
    for (int i = 0; i < 2; i++) {
        players[i].type = inPlayers[i].type;
        result = ggpo_add_player(ggpo, inPlayers + i, &players[i].handle);
        if (!GGPO_SUCCEEDED(result)) {
            spdlog::error("GGPO session could not add player: {}", (int)result);
            AbortGgpoMatch("GGPO could not add players — return to lobby and Ready again.");
            return;
        }

        if (players[i].type == GGPO_PLAYERTYPE_LOCAL) {
            const auto delayResult = ggpo_set_frame_delay(ggpo, players[i].handle, frameDelay);
            matchTelemetry.AppliedDelay(frameDelay, GGPO_SUCCEEDED(delayResult));
            localPlayerHandle = players[i].handle;
            localPlayerIdx = i;
        }
    }
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
        }
    }

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
    int ping = -1;
    for (int side = 0; side < 2; ++side) if (players[side].type == GGPO_PLAYERTYPE_REMOTE) {
        GGPONetworkStats stats{};
        if (GGPO_SUCCEEDED(ggpo_get_network_stats(ggpo, players[side].handle, &stats))) ping = stats.network.ping;
        break;
    }
    matchTelemetry.Sample(now, ping);
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
        AbortGgpoMatch("Netplay sync failed. The match has ended.");
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
        AbortGgpoMatch("Netplay sync failed. The match has ended.");
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
    AbortGgpoMatch("Netplay rollback buffer full. The match has ended.");
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
        break;
    case GGPO_EVENTCODE_RUNNING:
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
            sf4e::NetplayFacade::PushAlert("Connection unstable. Playing on prediction.", sf4e::NoticeSeverity::Warning);
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
            sf4e::NetplayFacade::PushAlert("Connection restored.", sf4e::NoticeSeverity::Info);
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
            spdlog::info("GGPO: spectator handle {} disconnected; fight continues", info->u.disconnected.player);
            sf4e::NetplayFacade::PushAlert("A spectator disconnected.", sf4e::NoticeSeverity::Info);
            break;
        }
        if (system) {
            *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
        }
        simGate.OnConnectionResumed(); // close any open warning episode
        simGate.OnBattleClosing();     // the gate must not report RUNNING for a dead peer
        s_disconnectTimeoutMs = 0;
        sf4e::NetplayFacade::PushAlert(
            localPlayerHandle == GGPO_INVALID_HANDLE ? "The match connection was lost." : "Opponent disconnected. The match is over.",
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

fSystem::SaveState::SaveState() {
    // There are at least 88 keys in every save state. The upper bound
    // is unclear, but we can minimize memory allocation delays by
    // reserving the lower bound.
    keys.reserve(88);
    // Sound records: clear() keeps this capacity, so after the first save of
    // a battle these never allocate again.
    criPlayerState.reserve(64);
    managerState.reserve(8);
}

std::map<int, std::pair<StateSnapshot, fSystem::StateSnapshotMeta>> fSystem::snapshotMap;
fSystem::HashCheckpoint fSystem::hashCheckpoints[fSystem::NUM_HASH_CHECKPOINTS];

// Computes the v2 semantic hashes for the current frame. Coverage is
// deliberately conservative: the frame counter, battle-flow numeric state,
// and per-character semantic values read through engine getters (the same
// values the legacy snapshot exchanges, which are known deterministic
// across peers, plus action id, action frame, posture and time scale).
// Explicitly EXCLUDED: the battle-flow function pointers,
// the raw GameManager block (shallow pointer fields), GameMementoKey bytes,
// sound maps (process-local pointer keys), RNG (the evolving RNG state has
// not been located; the match seed alone is not it), and all
// presentation/log/overlay state.
fSystem::SemanticHashes fSystem::ComputeSemanticHashes(rSystem* src) {
    using sf4e::statehash::Hasher;
    SemanticHashes out;

    FixedPoint* numFrames = rSystem::GetNumFramesSimulated_FixedPoint(src);

    Hasher flow;
    flow.Fixed(numFrames->fractional, numFrames->integral);
    flow.U32(*rSystem::staticVars.CurrentBattleFlow);
    flow.U32(*rSystem::staticVars.PreviousBattleFlow);
    flow.U32(*rSystem::staticVars.CurrentBattleFlowSubstate);
    flow.U32(*rSystem::staticVars.PreviousBattleFlowSubstate);
    flow.Fixed(
        rSystem::staticVars.CurrentBattleFlowFrame->fractional,
        rSystem::staticVars.CurrentBattleFlowFrame->integral
    );
    flow.Fixed(
        rSystem::staticVars.CurrentBattleFlowSubstateFrame->fractional,
        rSystem::staticVars.CurrentBattleFlowSubstateFrame->integral
    );
    flow.Fixed(
        rSystem::staticVars.PreviousBattleFlowFrame->fractional,
        rSystem::staticVars.PreviousBattleFlowFrame->integral
    );
    flow.Fixed(
        rSystem::staticVars.PreviousBattleFlowSubstateFrame->fractional,
        rSystem::staticVars.PreviousBattleFlowSubstateFrame->integral
    );
    out.flow = flow.Value();

    CharaActor::__publicMethods& methods = CharaActor::publicMethods;
    CharaUnit* lpCharaUnit = (src->*rSystem::publicMethods.GetCharaUnit)();
    for (int i = 0; i < 2; i++) {
        CharaActor* a = (lpCharaUnit->*CharaUnit::publicMethods.GetActorByIndex)(i);
        Hasher ch;
        ch.I32((a->*methods.GetStatus)());
        ch.I32((a->*methods.GetCurrentSide)());
        float* rootPos = (a->*methods.GetCurrentRootPosition)();
        for (int c = 0; c < 4; c++) {
            ch.F32(rootPos[c]);
        }
        FixedPoint fp;
        (a->*methods.GetVitalityAmt_FixedPoint)(&fp);          ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetVitalityMax_FixedPoint)(&fp);          ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetRevengeAmt_FixedPoint)(&fp);           ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetRevengeMax_FixedPoint)(&fp);           ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetRecoverableVitalityAmt_FixedPoint)(&fp); ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetRecoverableVitalityMax_FixedPoint)(&fp); ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetSuperComboAmt_FixedPoint)(&fp);        ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetSuperComboMax_FixedPoint)(&fp);        ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetSCTimeAmt_FixedPoint)(&fp);            ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetSCTimeMax_FixedPoint)(&fp);            ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetUCTimeAmt_FixedPoint)(&fp);            ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetUCTimeMax_FixedPoint)(&fp);            ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetComboDamage)(&fp);                     ch.Fixed(fp.fractional, fp.integral);
        (a->*methods.GetDamage)(&fp);                          ch.Fixed(fp.fractional, fp.integral);
        // Action timing (v0.8.6): the running move, how far into it the
        // character is, its posture, and the side's time scale (hitstop and
        // slowdown). A replay that keeps positions and health but lands a
        // move on a different frame now differs here.
        ch.I32((a->*methods.GetActionID)());
        (a->*methods.GetActionFrame)(&fp);                     ch.Fixed(fp.fractional, fp.integral);
        ch.I32((a->*methods.GetActionPosture)());
        (src->*rSystem::publicMethods.GetUnitTimeScale_Fixed)(&fp, i); ch.Fixed(fp.fractional, fp.integral);
        out.chara[i] = ch.Value();
    }

    Hasher overall;
    overall.U64(out.flow);
    overall.U64(out.chara[0]);
    overall.U64(out.chara[1]);
    out.overall = overall.Value();
    return out;
}

void fSystem::CaptureHashCheckpoint(rSystem* src) {
    // The engine counter is a signed 16-bit field and wraps during a long
    // match. GGPO's save callback uses a monotonic frame identity; spectators
    // have no save callback, so the confirmed input boundary plus one is the
    // equivalent state frame.
    int stateFrame = lastGgpoSaveFrame;
    if (localPlayerHandle == GGPO_INVALID_HANDLE) {
        int confirmed = -1;
        stateFrame = ggpo && GGPO_SUCCEEDED(ggpo_get_last_confirmed_frame(ggpo, &confirmed))
            ? sf4e::statehash::SpectatorCheckpointStateFrame(confirmed) : -1;
    }
    const int ringIndex = sf4e::statehash::CheckpointRingIndex(stateFrame);
    if (ringIndex < 0) {
        return;
    }
    HashCheckpoint& slot =
        hashCheckpoints[ringIndex];
    // Rollback resimulation legitimately re-captures a frame with corrected
    // values. The GGPO input-confirmation boundary gates exchange, rather
    // than elapsed engine frames (which have a different frame origin).
    sf4e::statehash::PrepareCheckpointIdentity(slot.frameIdx,slot.ggpoStateFrame,slot.sent,stateFrame);
    {
        diag::ScopedTimer _hashTimer(diag::OP_SEMANTIC_HASH);
        slot.hashes = ComputeSemanticHashes(src);
    }
    slot.valid = true;
}

fSystem::HashCheckpoint* fSystem::FindHashCheckpoint(int frameIdx) {
    const int ringIndex = sf4e::statehash::CheckpointRingIndex(frameIdx);
    if (ringIndex < 0) {
        return nullptr;
    }
    HashCheckpoint& slot =
        hashCheckpoints[ringIndex];
    return (slot.valid && slot.frameIdx == frameIdx) ? &slot : nullptr;
}

void fSystem::ClearHashCheckpoints() {
    for (int i = 0; i < NUM_HASH_CHECKPOINTS; i++) {
        hashCheckpoints[i] = HashCheckpoint();
    }
}

void fSystem::CaptureSnapshot(rSystem* src) {
    int frameIdx = rSystem::GetNumFramesSimulated_FixedPoint(src)->integral;

    // Only capture snapshots every second.
    if (frameIdx % 60 != 0) {
        return;
    }

    auto iter = snapshotMap.find(frameIdx);
    if (iter != snapshotMap.end()) {
        snapshotMap.erase(iter);
    }
    // Entries are normally retired by SessionClient::Step once sent and
    // confirmed. When the control plane is lost that never happens, and the
    // signed 16-bit engine counter wraps in a long match, so keep the map
    // bounded to the last ten seconds of checkpoints.
    for (auto old = snapshotMap.begin(); old != snapshotMap.end();) {
        if (old->first > frameIdx || frameIdx - old->first > 600) {
            old = snapshotMap.erase(old);
        }
        else {
            ++old;
        }
    }

    StateSnapshot snapshot;
    snapshot.frameIdx = frameIdx;

    CharaActor::__publicMethods& methods = CharaActor::publicMethods;
    CharaUnit* lpCharaUnit = (src->*rSystem::publicMethods.GetCharaUnit)();
    for (int i = 0; i < 2; i++) {
        CharaActor* a = (lpCharaUnit->*CharaUnit::publicMethods.GetActorByIndex)(i);
        memcpy_s(
            snapshot.chara[i].rootPos,
            sizeof(float) * 4,
            (a->*methods.GetCurrentRootPosition)(),
            sizeof(float) * 4
        );
        snapshot.chara[i].status = (a->*methods.GetStatus)();
        snapshot.chara[i].side = (a->*methods.GetCurrentSide)();

        (a->*methods.GetVitalityAmt_FixedPoint)(&snapshot.chara[i].vit);
        (a->*methods.GetVitalityMax_FixedPoint)(&snapshot.chara[i].vitmax);
        (a->*methods.GetRevengeAmt_FixedPoint)(&snapshot.chara[i].revenge);
        (a->*methods.GetRevengeMax_FixedPoint)(&snapshot.chara[i].revengemax);
        (a->*methods.GetRecoverableVitalityAmt_FixedPoint)(&snapshot.chara[i].recoverable);
        (a->*methods.GetRecoverableVitalityMax_FixedPoint)(&snapshot.chara[i].recoverablemax);
        (a->*methods.GetSuperComboAmt_FixedPoint)(&snapshot.chara[i].super);
        (a->*methods.GetSuperComboMax_FixedPoint)(&snapshot.chara[i].supermax);
        (a->*methods.GetSCTimeAmt_FixedPoint)(&snapshot.chara[i].sctimeamt);
        (a->*methods.GetSCTimeMax_FixedPoint)(&snapshot.chara[i].sctimemax);
        (a->*methods.GetUCTimeAmt_FixedPoint)(&snapshot.chara[i].uctime);
        (a->*methods.GetUCTimeMax_FixedPoint)(&snapshot.chara[i].uctimemax);
        (a->*methods.GetComboDamage)(&snapshot.chara[i].combodamage);
        (a->*methods.GetDamage)(&snapshot.chara[i].damage);
    }
    StateSnapshotMeta meta{ false, false };
    snapshotMap.emplace(frameIdx, std::make_pair(std::move(snapshot), meta));
}

// Looks up `key` in a flat save record. Records are appended in
// shadowManagerMap order, so the cursor makes the common case O(1); the
// scan covers a changed adapter set. Returns null when the key was not saved.
template <class Key, class Value>
static Value* FindSavedEntry(std::vector<std::pair<Key, Value>>& entries, Key key, size_t& cursor) {
    const size_t count = entries.size();
    for (size_t probe = 0; probe < count; probe++) {
        const size_t index = (cursor + probe) % count;
        if (entries[index].first == key) {
            cursor = index + 1;
            return &entries[index].second;
        }
    }
    return nullptr;
}

void CopyIntoPlace(fSystem::SaveState* src) {
    rSystem* system = rSystem::staticMethods.GetSingleton();

    *rSystem::staticVars.CurrentBattleFlow = src->d.CurrentBattleFlow;
    *rSystem::staticVars.PreviousBattleFlow = src->d.PreviousBattleFlow;
    *rSystem::staticVars.CurrentBattleFlowSubstate = src->d.CurrentBattleFlowSubstate;
    *rSystem::staticVars.PreviousBattleFlowSubstate = src->d.PreviousBattleFlowSubstate;
    *rSystem::staticVars.CurrentBattleFlowFrame = src->d.CurrentBattleFlowFrame;
    *rSystem::staticVars.CurrentBattleFlowSubstateFrame = src->d.CurrentBattleFlowSubstateFrame;
    *rSystem::staticVars.PreviousBattleFlowFrame = src->d.PreviousBattleFlowFrame;
    *rSystem::staticVars.PreviousBattleFlowSubstateFrame = src->d.PreviousBattleFlowSubstateFrame;
    *rSystem::staticVars.BattleFlowSubstateCallable_aa9258 = src->d.BattleFlowSubstateCallable_aa9258;
    *rSystem::staticVars.BattleFlowCallback_CallEveryFrame_aa9254 = src->d.BattleFlowCallback_CallEveryFrame_aa9254;
    memcpy_s((system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager), &src->d.gameManager, sizeof(GameManager));

    // Restore only what the state recorded. An adapter or manager that did
    // not exist at save time is left alone; the old map-based lookup
    // inserted and restored a zeroed entry for it.
    {
        size_t adapterCursor = 0;
        size_t managerCursor = 0;
        for (
            auto managerIter = fSoundPlayerManager::shadowManagerMap.begin();
            managerIter != fSoundPlayerManager::shadowManagerMap.end();
            managerIter++) {
            rSoundPlayerManager* stubManager = managerIter->first;
            rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(stubManager);
            for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
                const auto* record = FindSavedEntry(src->criPlayerState, &adapters[i], adapterCursor);
                if (record) {
                    fSoundPlayerManager::adapterToCurrentSound[&adapters[i]] = *record;
                }
            }
            auto* pool = FindSavedEntry(src->managerState, stubManager, managerCursor);
            if (pool) {
                sf4e::Platform::SoundObjectPool<4>::Load(rSoundPlayerManager::GetAdapterPool(stubManager), pool);
            }
        }
    }

    // Place each memento key back into its position.
    for (auto iter = src->keys.begin(); iter != src->keys.end(); iter++) {
        *iter->first = iter->second;
    }

    // Force the system to reload from the replaced mementos.
    fSystem::RestoreAllFromInternalMementos(system, &GGPO_MEMENTO_ID);
}

void Clear(fSystem::SaveState* victim) {
    // Only release payloads this state still has a claim on. A state whose
    // ownership was handed back (see SaveState::Free) holds stale copies
    // whose payloads now belong to the live keys; clearing through them
    // would be a double free.
    if (victim->ownsKeys) {
        for (auto iter = victim->keys.begin(); iter != victim->keys.end(); iter++) {
            if (iter->first) {
                (iter->first->*rKey::publicMethods.ClearKey)();
                memset(iter->first, 0, sizeof(rKey));
            }
        }
    }
    victim->keys.clear();
    victim->ownsKeys = true;

    // Restore all non-memento-key state to a sane default. Slot reuse must
    // also reset frame metadata so a stale callback identity can never be
    // attributed to a new frame.
    victim->used = false;
    victim->simulationFrame = -1;
    victim->ggpoFrame = -1;
    victim->d.CurrentBattleFlow = 0;
    victim->d.PreviousBattleFlow = 0;
    victim->d.CurrentBattleFlowSubstate = 0;
    victim->d.PreviousBattleFlowSubstate = 0;
    victim->d.CurrentBattleFlowFrame = { 0, 0 };
    victim->d.CurrentBattleFlowSubstateFrame = { 0, 0 };
    victim->d.PreviousBattleFlowFrame = { 0, 0 };
    victim->d.PreviousBattleFlowSubstateFrame = { 0, 0 };
    victim->d.BattleFlowSubstateCallable_aa9258 = nullptr;
    victim->d.BattleFlowCallback_CallEveryFrame_aa9254 = nullptr;
    victim->criPlayerState.clear();
    victim->managerState.clear();
}

void fSystem::SaveState::Reclaim(SaveState* victim, const char* reason, int slotIndex) {
    if (!victim->used && victim->keys.empty()) {
        return;
    }
    spdlog::warn(
        "SaveState: reclaiming leaked slot {} ({}) used={} keys={} simFrame={} ggpoFrame={}",
        slotIndex,
        reason ? reason : "?",
        victim->used,
        victim->keys.size(),
        victim->simulationFrame,
        victim->ggpoFrame
    );
    // Drop the records without engine calls. At the points that call this
    // (session start, post-teardown sweep) the battle objects the keys point
    // at are either gone or owned by a fresh battle, so ClearKey through
    // those pointers is exactly what must not happen.
    victim->ownsKeys = false;
    Clear(victim);
}

static bool EnvironmentFlagSet(const char* name) {
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    return length > 0 && length < sizeof(value) && value[0] == '1';
}

// Read once per process. SF4E_LEGACY_SAVESTATE_FREE=1 selects the v0.8.5
// round-trip release for A/B comparison; SF4E_SAVESTATE_FREE_VERIFY=1 checks
// that each release leaves the live game state untouched.
struct SaveStateFreePolicy {
    bool legacyRoundTrip;
    bool verify;
};

static const SaveStateFreePolicy& FreePolicy() {
    static const SaveStateFreePolicy policy = {
        EnvironmentFlagSet("SF4E_LEGACY_SAVESTATE_FREE"),
        EnvironmentFlagSet("SF4E_SAVESTATE_FREE_VERIFY"),
    };
    return policy;
}

static const char* SaveStateFreePathName() {
    return FreePolicy().legacyRoundTrip ? "legacy_round_trip" : "swap";
}

static void LogSaveStateFreePolicy() {
    spdlog::info(
        "SaveState: free path={} verify={}",
        FreePolicy().legacyRoundTrip ? "legacy_round_trip" : "swap",
        FreePolicy().verify
    );
}

// Everything a release must not change: the semantic gameplay hash, the
// battle-flow globals, the GameManager block and, for the swap path, every
// live memento key byte. The legacy round trip hands the temporary save's
// payloads to the live keys by design, so its key bytes always differ.
static uint64_t HashLiveStateForFreeCheck(bool includeKeys) {
    sf4e::statehash::Hasher hasher;
    rSystem* system = rSystem::staticMethods.GetSingleton();
    if (!system) {
        return 0;
    }
    hasher.U64(fSystem::ComputeSemanticHashes(system).overall);
    const auto bytes = [&](const void* data, size_t size) {
        const auto* cursor = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++) {
            hasher.U8(cursor[i]);
        }
    };
    bytes(rSystem::staticVars.CurrentBattleFlow, sizeof(DWORD));
    bytes(rSystem::staticVars.CurrentBattleFlowSubstate, sizeof(DWORD));
    bytes(rSystem::staticVars.CurrentBattleFlowFrame, sizeof(FixedPoint));
    bytes((system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager));
    if (includeKeys) {
        for (rKey* key : fKey::trackedKeys) {
            hasher.U32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key)));
            bytes(key, sizeof(rKey));
        }
    }
    return hasher.Value();
}

// Default release. The engine's ClearKey (0x52F3D0) uses the live mementoable
// object only to find its vtable, and every memento destructor it reaches
// touches memento-owned memory alone (docs/SAVESTATE_FREE.md). So the victim's
// key is installed just long enough to release it, and the live key is put
// back. No temporary save and no memento restore is needed.
static void FreeBySwap(fSystem::SaveState* victim) {
    diag::ScopedTimer _t(diag::OP_FREE_SWAP);
    if (victim->ownsKeys) {
        for (auto& entry : victim->keys) {
            if (!entry.first) {
                continue;
            }
            const rKey live = *entry.first;
            *entry.first = entry.second;
            // The original engine function, not the tracking detour: the
            // address still belongs to a live, tracked key.
            (entry.first->*rKey::publicMethods.ClearKey)();
            *entry.first = live;
        }
    }
    // The payloads were released above; Clear must only drop the records.
    victim->ownsKeys = false;
    Clear(victim);
}

void fSystem::SaveState::Free(SaveState* victim) {
    diag::ScopedTimer _freeTimer(diag::OP_FREE_TOTAL);
    AssertSaveStateThreadAffinity();
    const auto& policy = FreePolicy();
    const uint64_t before = policy.verify ? HashLiveStateForFreeCheck(!policy.legacyRoundTrip) : 0;
    if (policy.legacyRoundTrip) {
        FreeByRoundTrip(victim);
    }
    else {
        FreeBySwap(victim);
    }
    if (policy.verify) {
        const uint64_t after = HashLiveStateForFreeCheck(!policy.legacyRoundTrip);
        if (after != before) {
            spdlog::error(
                "SaveState: releasing a state changed live game state (path={} simFrame={} before={:016x} after={:016x})",
                policy.legacyRoundTrip ? "legacy_round_trip" : "swap",
                rSystem::staticMethods.GetSingleton()
                    ? (int)rSystem::GetNumFramesSimulated_FixedPoint(rSystem::staticMethods.GetSingleton())->integral
                    : -1,
                before,
                after
            );
        }
    }
    if (diag::Enabled()) {
        uint32_t occupied = 0;
        for (int i = 0; i < NUM_SAVE_STATES; i++) {
            if (saveStates[i].used) {
                occupied++;
            }
        }
        diag::G().occupiedSaveSlots.Update(occupied);
    }
}

// v0.8.5 release, kept for SF4E_LEGACY_SAVESTATE_FREE A/B comparison.
void fSystem::SaveState::FreeByRoundTrip(SaveState* victim) {
    SaveState tmp;

    {
        diag::ScopedTimer _t(diag::OP_FREE_TMP_SAVE);
        SaveState::Save(&tmp, true);
    }

    // Calls to clear SF4's mementos delegate those calls to the mementoable
    // object. If the mementoable object pointer isn't valid, the key can't
    // be cleared. This isn't relevant to SF4's training mode, because clearing
    // is only ever done on re-initialization after a save, but manually
    // clearing keys when releasing the state is necessary for GGPO to avoid
    // memory leaks.
    //
    // Copy the victim state into the engine. Once the victim state is copied,
    // the mementoable object pointers in each key are valid, and each key can
    // be safely cleared.
    {
        diag::ScopedTimer _t(diag::OP_FREE_VICTIM_INSTALL);
        CopyIntoPlace(victim);
    }
    {
        diag::ScopedTimer _t(diag::OP_FREE_CLEAR);
        Clear(victim);
    }

    // Restore the state at the start of the function.
    //
    // CopyIntoPlace copies each key struct back into its live slot, which
    // hands ownership of every memento payload back to the engine. `tmp`
    // still holds identical copies pointing at those same payloads, so its
    // ownership record is now stale and must be dropped WITHOUT calling
    // ClearKey — releasing it through the engine would free the payloads the
    // live keys just took back. Being short-lived is not sufficient: `tmp`
    // has no destructor, and CloseBattle calls Free in a loop, so a stale
    // claim here is freed again by the next iteration's Clear().
    {
        diag::ScopedTimer _t(diag::OP_FREE_LIVE_RESTORE);
        CopyIntoPlace(&tmp);
        tmp.ownsKeys = false;
        tmp.keys.clear();
    }
}

void fSystem::SaveState::Load(SaveState* src) {
    diag::ScopedTimer _loadTimer(diag::OP_LOAD_TOTAL);
    AssertSaveStateThreadAffinity();
    // Main-thread scratch (asserted above). Kept across loads so a rollback
    // does not allocate; clear() retains the capacity.
    static std::vector<std::pair<rKey*, rKey>> tmpVec;
    tmpVec.clear();
    tmpVec.reserve(fKey::trackedKeys.size());

    // Loading a state abandons the current timeline, and with it any
    // stop intents queued by frames that are about to be re-simulated
    // (or, for training-mode loads, discarded outright). Re-simulation
    // re-queues every stop that survives in the corrected timeline, and
    // the sync step's "shouldn't be playing" pass covers sounds that die
    // with the abandoned one, so stale entries must not linger here-
    // they'd stop (and audibly restart) sounds that are still supposed
    // to be playing. Note this deliberately does NOT happen in
    // CopyIntoPlace: SaveState::Free round-trips through that with the
    // current timeline still live, and must preserve the queue.
    if (fSoundPlayerManager::bUsePureSounds) {
        for (auto& queue : fSoundPlayerManager::queuedStops) {
            queue.second.clear();
        }
    }

    // Copy and zero all currently tracked keys. It's possible that the
    // initialization detour started tracking keys that were only
    // initialized after the save state was created.
    {
        diag::ScopedTimer _t(diag::OP_LOAD_KEY_BACKUP);
        for (auto iter = fKey::trackedKeys.begin(); iter != fKey::trackedKeys.end(); iter++) {
            tmpVec.push_back(std::make_pair(*iter, **iter));
            memset(*iter, 0, sizeof(rKey));
        }
    }

    {
        diag::ScopedTimer _t(diag::OP_LOAD_COPY_INTO_PLACE);
        CopyIntoPlace(src);
    }
    // Preserve samples on the retained timeline and discard speculative
    // outcomes after the restored GGPO state. Corrected saves refill them.
    // Free() only round-trips storage and must not rewind this history.
    // Only GGPO saves carry a frame; a training or stress load has none and
    // must not wipe the timeline.
    if (src->ggpoFrame >= 0) {
        s_nativeResultTimeline.Rewind(src->ggpoFrame);
    }

    diag::ScopedTimer _restoreTimer(diag::OP_LOAD_RESTORE_KEYS);

    // Zero the keys that were injected by the load.
    //
    // If the memento key data from the source state were left in the key,
    // the next save would result in invalidating the memento key data and
    // the `SaveState()` pointing at invalid memory. It's also possible
    // that the keys in the loaded state are not a proper subset of the
    // keys that existed in the state when load was called, so this
    // function can't iterate over the existing tracked keys.
    for (auto iter = src->keys.begin(); iter != src->keys.end(); iter++) {
        if (iter->first) {
            memset(iter->first, 0, sizeof(rKey));
        }
    }

    // Finally, restore the original state of all tracked keys.
    for (auto iter = tmpVec.begin(); iter != tmpVec.end(); iter++) {
        *iter->first = iter->second;
    }
}

void fSystem::SaveState::Save(SaveState* dst, bool temporary) {
    diag::ScopedTimer _saveTimer(temporary ? -1 : diag::OP_SAVE_TOTAL);
    AssertSaveStateThreadAffinity();
    rSystem* system = rSystem::staticMethods.GetSingleton();

    // Saving into a slot that still holds records would append to them,
    // overwriting the only pointers to the previous payloads and leaking
    // every one. This was assert-only, so in release it corrupted silently.
    // Recover by releasing the slot properly first: the payloads are still
    // owned here, so Free (not Reclaim) is the correct release.
    if (!dst->keys.empty()) {
        spdlog::error(
            "SaveState: saving into a dirty slot (keys={} used={} simFrame={} ggpoFrame={}); releasing first",
            dst->keys.size(),
            dst->used,
            dst->simulationFrame,
            dst->ggpoFrame
        );
        assert(false && "SaveState::Save into a non-empty slot");
        SaveState::Free(dst);
    }

    dst->used = true;
    dst->ownsKeys = true;

    {
        diag::ScopedTimer _t(temporary ? -1 : diag::OP_SAVE_RECORD_MEMENTOS);
        RecordAllToInternalMementos(system, &GGPO_MEMENTO_ID);
    }
    {
        diag::ScopedTimer _t(temporary ? -1 : diag::OP_SAVE_COPY_KEYS);
        for (auto iter = fKey::trackedKeys.begin(); iter != fKey::trackedKeys.end(); iter++) {
            dst->keys.emplace_back(*iter, **iter);

            // If we leave the data in the source key, reinitialization
            // of the source key will end up freeing _our_ data. Make
            // absolutely sure to zero the source key. Ideally, we could
            // just replace the key's state with the state the key had
            // before the call to RecordAll... but the mementos won't
            // be tracked until after that call.
            memset(*iter, 0, sizeof(rKey));
        }
    }

    {
        diag::ScopedTimer _t(temporary ? -1 : diag::OP_SAVE_SOUND);
        for (
            auto managerIter = Sound::SoundPlayerManager::shadowManagerMap.begin();
            managerIter != Sound::SoundPlayerManager::shadowManagerMap.end();
            managerIter++) {
            rSoundPlayerManager* stubManager = managerIter->first;
            rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(stubManager);
            for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
                // Read-only lookup: an adapter with no current sound is
                // recorded as an empty request rather than inserted into the
                // live map from the save path.
                const auto current = fSoundPlayerManager::adapterToCurrentSound.find(&adapters[i]);
                dst->criPlayerState.emplace_back(
                    &adapters[i],
                    current != fSoundPlayerManager::adapterToCurrentSound.end()
                        ? current->second
                        : Sound::SoundPlayerManager::DeferredSoundRequest()
                );
            }
            dst->managerState.emplace_back(stubManager, Platform::SoundObjectPool<4>::SaveState());
            Platform::SoundObjectPool<4>::Save(rSoundPlayerManager::GetAdapterPool(stubManager), &dst->managerState.back().second);
        }
    }

    // The constructor reserves 88 keys (the observed lower bound). Record
    // growth past that once per process so live telemetry can establish
    // the real stable count before any capacity change is made.
    if (dst->keys.size() > 88) {
        static bool s_warnedKeyGrowth = false;
        if (!s_warnedKeyGrowth) {
            s_warnedKeyGrowth = true;
            spdlog::warn(
                "SaveState: key count {} exceeds the 88-key reservation (capacity {})",
                dst->keys.size(),
                dst->keys.capacity()
            );
        }
    }

    if (!temporary && diag::Enabled()) {
        diag::RollbackDiagnostics& d = diag::G();
        d.trackedKeys.Update((uint32_t)fKey::trackedKeys.size());
        d.saveKeyVectorSize.Update((uint32_t)dst->keys.size());
        d.saveKeyVectorCapacity.Update((uint32_t)dst->keys.capacity());
        d.soundManagers.Update((uint32_t)Sound::SoundPlayerManager::shadowManagerMap.size());
        d.soundAdapters.Update((uint32_t)fSoundPlayerManager::adapterToCurrentSound.size());
        d.criPlayerStateSize.Update((uint32_t)dst->criPlayerState.size());
        d.managerStateSize.Update((uint32_t)dst->managerState.size());
    }

    diag::ScopedTimer _globalsTimer(temporary ? -1 : diag::OP_SAVE_GLOBALS);
    dst->d.CurrentBattleFlow = *rSystem::staticVars.CurrentBattleFlow;
    dst->d.PreviousBattleFlow = *rSystem::staticVars.PreviousBattleFlow;
    dst->d.CurrentBattleFlowSubstate = *rSystem::staticVars.CurrentBattleFlowSubstate;
    dst->d.PreviousBattleFlowSubstate = *rSystem::staticVars.PreviousBattleFlowSubstate;
    dst->d.CurrentBattleFlowFrame = *rSystem::staticVars.CurrentBattleFlowFrame;
    dst->d.CurrentBattleFlowSubstateFrame = *rSystem::staticVars.CurrentBattleFlowSubstateFrame;
    dst->d.PreviousBattleFlowFrame = *rSystem::staticVars.PreviousBattleFlowFrame;
    dst->d.PreviousBattleFlowSubstateFrame = *rSystem::staticVars.PreviousBattleFlowSubstateFrame;
    dst->d.BattleFlowSubstateCallable_aa9258 = *rSystem::staticVars.BattleFlowSubstateCallable_aa9258;
    dst->d.BattleFlowCallback_CallEveryFrame_aa9254 = *rSystem::staticVars.BattleFlowCallback_CallEveryFrame_aa9254;

    memcpy_s(&dst->d.gameManager, sizeof(GameManager), (system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager));
}
