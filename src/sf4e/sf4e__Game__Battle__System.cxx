#include <algorithm>
#include <string.h>
#include <utility>
#include <vector>

#include <windows.h>
#include <detours/detours.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
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
#include "../Dimps/Dimps__Game__Battle__Vfx.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"

#include "../common/sf4e__RollbackDiagnostics.hxx"
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
#include "../session/sf4e__GgpoRelay.hxx"

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

static bool s_updateAllowedBeforeGgpoInterrupt = true;

// Emits the complete diagnostics summary through the normal log. Called at
// match end / abort, before battle state is torn down.
static void EmitRollbackDiagSummary(const char* label) {
    if (!diag::Enabled()) {
        return;
    }
    static char s_diagBuf[8192];
    size_t n = diag::G().FormatSummary(s_diagBuf, sizeof(s_diagBuf), label);
    if (n) {
        spdlog::info("\n{}", s_diagBuf);
    }
}

bool fSystem::bHaltAfterNext = false;
bool fSystem::bUpdateAllowed = true;
bool fSystem::bGgpoConnectionInterrupted = false;
int fSystem::nExtraFramesToSimulate = 0;
int fSystem::nNextBattleStartFlowTarget = -1;
int fSystem::nRandomizeLocalInputsEveryXFramesInGGPO = 0;

GGPOPlayerHandle fSystem::localPlayerHandle = GGPO_INVALID_HANDLE;
GGPOSession* fSystem::ggpo = nullptr;
fSystem::PlayerConnectionInfo fSystem::players[MAX_SF4E_PROTOCOL_USERS];
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

    return (this->*rSystem::mementoableMethods.RestoreFromMemento)(m, id);
}

void fSystem::BattleUpdate() {
    rSystem* _this = (rSystem*)this;
    rSystem::__publicMethods& sysMethods = rSystem::publicMethods;
    rPadSystem* p = rPadSystem::staticMethods.GetSingleton();
    rPadSystem::__publicMethods& padMethods = rPadSystem::publicMethods;
    static int nLastRandomInputFrame = -1;
    static fPadSystem::Inputs randomInputs = { 0, 0 };

    diag::ScopedTimer _updateTimer(diag::OP_DETOURED_BATTLE_UPDATE);

    if (!bUpdateAllowed) {
        if (ggpo && diag::Enabled()) {
            diag::G().RecordSkip(diag::SKIP_UPDATE_GATE, diag::NowMs());
        }
        return;
    }

    if (ggpo && *rSystem::staticVars.CurrentBattleFlow != BF__IDLE) {
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
                        inputs = { (p->*padMethods.GetButtons_MappedOn)(i), (p->*padMethods.GetButtons_RawOn)(i) };
                    }
                    {
                        diag::ScopedTimer _t(diag::OP_ADD_LOCAL_INPUT);
                        result = ggpo_add_local_input(ggpo, players[i].handle, &inputs, sizeof(fPadSystem::Inputs));
                    }
                    if (diag::Enabled()) {
                        diag::G().RecordGgpoResult(diag::CALL_ADD_LOCAL_INPUT, (int)result);
                        if (!GGPO_SUCCEEDED(result)) {
                            int skip = diag::SKIP_ADD_INPUT_ERROR;
                            if (result == GGPO_ERRORCODE_PREDICTION_THRESHOLD) {
                                skip = diag::SKIP_PREDICTION_THRESHOLD;
                            }
                            else if (result == GGPO_ERRORCODE_NOT_SYNCHRONIZED) {
                                skip = diag::SKIP_NOT_SYNCHRONIZED;
                            }
                            diag::G().RecordSkip(skip, diag::NowMs());
                        }
                    }
                    break;
                }
            }
        }

        if (GGPO_SUCCEEDED(result)) {
            fPadSystem::Inputs ggpoInputs[2] = { {0, 0}, {0, 0} };
            int disconnect_flags = 0;
            {
                diag::ScopedTimer _t(diag::OP_SYNC_INPUT);
                result = ggpo_synchronize_input(ggpo, (void*)ggpoInputs, sizeof(fPadSystem::Inputs) * 2, &disconnect_flags);
            }
            if (diag::Enabled()) {
                diag::G().RecordGgpoResult(diag::CALL_SYNC_INPUT, (int)result);
                if (!GGPO_SUCCEEDED(result)) {
                    diag::G().RecordSkip(diag::SKIP_SYNC_INPUT_ERROR, diag::NowMs());
                }
            }
            if (GGPO_SUCCEEDED(result)) {
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
                fPadSystem::playbackFrame = -1;
                GGPOErrorCode err;
                {
                    diag::ScopedTimer _t(diag::OP_ADVANCE_FRAME_API);
                    err = ggpo_advance_frame(ggpo);
                }
                if (diag::Enabled()) {
                    diag::G().RecordGgpoResult(diag::CALL_ADVANCE_FRAME, (int)err);
                }
                if (!GGPO_SUCCEEDED(err)) {
                    AbortGgpoMatch("Netplay sync failed — match ended.");
                }
                else {
                    if (diag::Enabled()) {
                        diag::G().OnFrameAdvanced(diag::NowMs());
                    }
                    if (fSoundPlayerManager::bUsePureSounds) {
                        fSoundPlayerManager::SyncState();
                    }
                    CaptureSnapshot(_this);
                }
            }
        }
    }
    else {
        if (fSoundPlayerManager::bUsePureSounds) {
            fSoundPlayerManager::SyncState();
        }
        (_this->*rSystem::publicMethods.BattleUpdate)();
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
    }
}

void fSystem::CloseBattle() {
    rSystem* _this = (rSystem*)this;
    if (ggpo) {
        // Decide defer *before* close so a prior spectator defer flag cannot
        // leave this session open across rematch.
        sf4e::NetplayFacade::NotifyMatchEnded();
        if (!sf4e::NetplayFacade::ShouldDeferGgpoClose()) {
            ggpo_close_session(ggpo);
            ggpo = nullptr;
            sf4e::GgpoRelay::Instance().Reset();
        }
        bGgpoConnectionInterrupted = false;
    }
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used) {
            SaveState::Free(&saveStates[i]);
        }
    }
    EmitRollbackDiagSummary("battle_close");
    (_this->*rSystem::publicMethods.CloseBattle)();

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

    if (extendedLoadRequest) {
        if (saveStates[0].used) {
            fSystem::SaveState::Load(&saveStates[0]);
        }
        extendedLoadRequest = false;
    }

    if (extendedSaveRequest) {
        if (saveStates[0].used) {
            fSystem::SaveState::Free(&saveStates[0]);
        }
        fSystem::SaveState::Save(&saveStates[0]);
        extendedSaveRequest = false;
    }

    (_this->*rSystem::publicMethods.SysMain_HandleTrainingModeFeatures)();
}

void fSystem::SysMain_UpdatePauseState() {
    if (!ggpo) {
        (this->*rSystem::publicMethods.SysMain_UpdatePauseState)();
    }
}

void fSystem::RestoreAllFromInternalMementos(rSystem* system, rKey::MementoID * id) {
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    CharaUnit* charaUnit = (CharaUnit*)(system->*GetUnitByIndex)(rSystem::U_CHARA);

    (system->*rSystem::publicMethods.RestoreFromInternalMementoKey)(id);
    (charaUnit->*CharaUnit::publicMethods.RestoreFromInternalMementoKey)(id);
    (
        ((EffectUnit*)(system->*GetUnitByIndex)(rSystem::U_EFFECT))->*
        EffectUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);

    (
        ((VfxUnit*)(system->*GetUnitByIndex)(rSystem::U_VFX))->*
        VfxUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);


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

    CharaActor::staticMethods.ResetAfterMemento((charaUnit->*CharaUnit::publicMethods.GetActorByIndex)(0));
    CharaActor::staticMethods.ResetAfterMemento((charaUnit->*CharaUnit::publicMethods.GetActorByIndex)(1));

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
    (system->*rSystem::publicMethods.RecordToInternalMementoKey)(id);

    (
        ((CharaUnit*)(system->*GetUnitByIndex)(rSystem::U_CHARA))->*
        CharaUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((EffectUnit*)(system->*GetUnitByIndex)(rSystem::U_EFFECT))->*
        EffectUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((VfxUnit*)(system->*GetUnitByIndex)(rSystem::U_VFX))->*
        VfxUnit::publicMethods.RecordToInternalMementoKey
        )(id);

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

void fSystem::AbortGgpoMatch(const char* reason) {
    if (reason && reason[0]) {
        spdlog::error("GGPO match abort: {}", reason);
        sf4e::NetplayFacade::PushAlert(reason);
    }
    EmitRollbackDiagSummary("abort");
    bUpdateAllowed = false;
    bGgpoConnectionInterrupted = false;
    if (ggpo) {
        ggpo_close_session(ggpo);
        ggpo = nullptr;
    }
    sf4e::GgpoRelay::Instance().Reset();
    sf4e::NetplayFacade::ClearBattleState();
    rSystem* system = rSystem::staticMethods.GetSingleton();
    if (system) {
        *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
    }
}

void fSystem::StartGGPO(GGPOPlayer* inPlayers, int numPlayers, int port, int frameDelay, DWORD rngSeed) {
    diag::InitFromEnvironment();
    diag::G().ResetForMatch(diag::NowMs());
    sf4e::NetplayFacade::CancelDeferredGgpoClose();
    if (ggpo) {
        spdlog::warn("StartGGPO: closing leftover GGPO session before rematch/restart");
        ggpo_close_session(ggpo);
        ggpo = nullptr;
    }
    sf4e::GgpoRelay::Instance().Reset();
    bGgpoConnectionInterrupted = false;
    localPlayerHandle = GGPO_INVALID_HANDLE;

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
            ggpo_set_frame_delay(ggpo, players[i].handle, frameDelay);
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
    bUpdateAllowed = false;
    fVsBattle::bTerminateOnNextLeftBattle = true;
    fVsBattle::bOverrideNextRandomSeed = true;
    fVsBattle::nextMatchRandomSeed = rngSeed;
}

void fSystem::StartSpectating(unsigned short localport, int num_players, char* host_ip, unsigned short host_port, DWORD rngSeed) {
    diag::InitFromEnvironment();
    diag::G().ResetForMatch(diag::NowMs());
    localPlayerHandle = GGPO_INVALID_HANDLE;
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
    bUpdateAllowed = false;
    fVsBattle::bTerminateOnNextLeftBattle = true;
    fVsBattle::bOverrideNextRandomSeed = true;
    fVsBattle::nextMatchRandomSeed = rngSeed;
}

bool fSystem::ggpo_begin_game_callback(const char*)
{
    return true;
}

bool fSystem::ggpo_advance_frame_callback(int)
{
    diag::ScopedTimer _cbTimer(diag::OP_ROLLBACK_CALLBACK);
    if (diag::Enabled()) {
        diag::G().OnRollbackCallback(diag::NowMs());
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
        AbortGgpoMatch("Netplay sync failed — match ended.");
        return true;
    }
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
        AbortGgpoMatch("Netplay sync failed — match ended.");
    }
    else {
        CaptureSnapshot(system);
    }

    fPadSystem::playbackFrame = -1;
    return true;
}

bool fSystem::ggpo_load_game_state_callback(unsigned char* buffer, int len)
{
    SaveState* state = (SaveState*)buffer;
    SaveState::Load(state);
    return true;
}

bool fSystem::ggpo_save_game_state_callback(unsigned char** buffer, int* len, int* checksum, int)
{
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
        *buffer = (unsigned char*)&saveStates[i];
        *checksum = 0;

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
    AbortGgpoMatch("Netplay rollback buffer full — match ended.");
    return false;
}

bool fSystem::ggpo_log_game_state(char* filename, unsigned char* buffer, int)
{
    return true;
}

void fSystem::ggpo_free_buffer(void* buffer)
{
    SaveState* victim = (SaveState*)buffer;
    SaveState::Free(victim);
}

bool fSystem::ggpo_on_event_callback(GGPOEvent* info) {
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
        bUpdateAllowed = true;
        spdlog::info("GGPO: Running");
        sf4e::NetplayFacade::NotifyGgpoSyncPhase(sf4e::GgpoSyncPhase::Running);
        break;
    case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
        spdlog::info("GGPO: GGPO_EVENTCODE_CONNECTION_INTERRUPTED");
        if (diag::Enabled()) {
            diag::G().OnConnectionInterrupted(diag::NowMs());
        }
        if (!bGgpoConnectionInterrupted) {
            bGgpoConnectionInterrupted = true;
            s_updateAllowedBeforeGgpoInterrupt = bUpdateAllowed;
            bUpdateAllowed = false;
        }
        sf4e::NetplayFacade::PushAlert("Connection unstable — waiting to reconnect...");
        break;
    case GGPO_EVENTCODE_CONNECTION_RESUMED:
        spdlog::info("GGPO: GGPO_EVENTCODE_CONNECTION_RESUMED");
        if (diag::Enabled()) {
            diag::G().OnConnectionResumed(diag::NowMs());
        }
        if (bGgpoConnectionInterrupted) {
            bGgpoConnectionInterrupted = false;
            bUpdateAllowed = s_updateAllowedBeforeGgpoInterrupt;
        }
        sf4e::NetplayFacade::PushAlert("Connection restored.");
        break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
        spdlog::info("GGPO: GGPO_EVENTCODE_DISCONNECTED_FROM_PEER");
        if (diag::Enabled()) {
            diag::G().OnTerminalDisconnect(diag::NowMs());
        }
        if (system) {
            *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
        }
        bGgpoConnectionInterrupted = false;
        sf4e::NetplayFacade::PushAlert("Opponent disconnected.");
        break;
    case GGPO_EVENTCODE_TIMESYNC:
        if (diag::Enabled()) {
            diag::G().OnTimesyncEvent(info->u.timesync.frames_ahead);
        }
        {
            diag::ScopedTimer _t(diag::OP_TIMESYNC_SLEEP);
            Sleep(1000 * info->u.timesync.frames_ahead / 60);
        }
        break;
    }
    return true;
}

fSystem::SaveState::SaveState() {
    // There are at least 88 keys in every save state. The upper bound
    // is unclear, but we can minimize memory allocation delays by
    // reserving the lower bound.
    keys.reserve(88);
}

std::map<int, std::pair<StateSnapshot, fSystem::StateSnapshotMeta>> fSystem::snapshotMap;

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

    for (
        auto managerIter = fSoundPlayerManager::shadowManagerMap.begin();
        managerIter != fSoundPlayerManager::shadowManagerMap.end();
        managerIter++) {
        rSoundPlayerManager* stubManager = managerIter->first;
        rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(stubManager);
        for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
            fSoundPlayerManager::adapterToCurrentSound[&adapters[i]] = src->criPlayerState[&adapters[i]];
        }
        sf4e::Platform::SoundObjectPool<4>::SaveState poolState;
        sf4e::Platform::SoundObjectPool<4>::Load(
            rSoundPlayerManager::GetAdapterPool(stubManager),
            &src->managerState[stubManager]
        );
    }

    // Place each memento key back into its position.
    for (auto iter = src->keys.begin(); iter != src->keys.end(); iter++) {
        *iter->first = iter->second;
    }

    // Force the system to reload from the replaced mementos.
    fSystem::RestoreAllFromInternalMementos(system, &GGPO_MEMENTO_ID);
}

void Clear(fSystem::SaveState* victim) {
    for (auto iter = victim->keys.begin(); iter != victim->keys.end(); iter++) {
        if (iter->first) {
            (iter->first->*rKey::publicMethods.ClearKey)();
            memset(iter->first, 0, sizeof(rKey));
        }
    }
    victim->keys.clear();

    // Restore all non-memento-key state to a sane default.
    victim->used = false;
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

void fSystem::SaveState::Free(SaveState* victim) {
    diag::ScopedTimer _freeTimer(diag::OP_FREE_TOTAL);
    SaveState tmp;

    {
        diag::ScopedTimer _t(diag::OP_FREE_TMP_SAVE);
        SaveState::Save(&tmp);
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

    // Restore the state at the start of the function. We don't need to
    // handle clearing the keys injected by this operation, because the
    // SaveState managing the keys is short-lived.
    {
        diag::ScopedTimer _t(diag::OP_FREE_LIVE_RESTORE);
        CopyIntoPlace(&tmp);
    }
}

void fSystem::SaveState::Load(SaveState* src) {
    diag::ScopedTimer _loadTimer(diag::OP_LOAD_TOTAL);
    std::vector<std::pair<rKey*, rKey>> tmpVec;

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

void fSystem::SaveState::Save(SaveState* dst) {
    diag::ScopedTimer _saveTimer(diag::OP_SAVE_TOTAL);
    rSystem* system = rSystem::staticMethods.GetSingleton();
    assert(dst->keys.empty());

    dst->used = true;

    {
        diag::ScopedTimer _t(diag::OP_SAVE_RECORD_MEMENTOS);
        RecordAllToInternalMementos(system, &GGPO_MEMENTO_ID);
    }
    {
        diag::ScopedTimer _t(diag::OP_SAVE_COPY_KEYS);
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
        diag::ScopedTimer _t(diag::OP_SAVE_SOUND);
        for (
            auto managerIter = Sound::SoundPlayerManager::shadowManagerMap.begin();
            managerIter != Sound::SoundPlayerManager::shadowManagerMap.end();
            managerIter++) {
            rSoundPlayerManager* stubManager = managerIter->first;
            rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(stubManager);
            for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
                dst->criPlayerState[&adapters[i]] = fSoundPlayerManager::adapterToCurrentSound[&adapters[i]];
            }
            Platform::SoundObjectPool<4>::SaveState poolState;
            Platform::SoundObjectPool<4>::Save(rSoundPlayerManager::GetAdapterPool(stubManager), &poolState);
            dst->managerState[stubManager] = poolState;
        }
    }

    if (diag::Enabled()) {
        diag::RollbackDiagnostics& d = diag::G();
        d.trackedKeys.Update((uint32_t)fKey::trackedKeys.size());
        d.saveKeyVectorSize.Update((uint32_t)dst->keys.size());
        d.saveKeyVectorCapacity.Update((uint32_t)dst->keys.capacity());
        d.soundManagers.Update((uint32_t)Sound::SoundPlayerManager::shadowManagerMap.size());
        d.soundAdapters.Update((uint32_t)fSoundPlayerManager::adapterToCurrentSound.size());
        d.criPlayerStateSize.Update((uint32_t)dst->criPlayerState.size());
        d.managerStateSize.Update((uint32_t)dst->managerState.size());
    }

    diag::ScopedTimer _globalsTimer(diag::OP_SAVE_GLOBALS);
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
