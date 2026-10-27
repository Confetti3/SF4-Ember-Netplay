// sf4e::Game::Battle::System: the System memento detours and the record and
// restore passes over every unit's internal memento key. Split from
// sf4e__Game__Battle__System.cxx.
#include "sf4e__Game__Battle__System__Internal.hxx"
#include "sf4e__MementoGuards.hxx"
#include "../common/MementoGuards.hxx"

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

// The battle's own key with an object but no metadata would be written
// through null by the record below (common/MementoGuards.hxx). What leaves it
// that way is not known yet, so say what the key held before emptying it.
static void RepairSystemMementoKey(rSystem* system) {
    rKey* key = rSystem::GetMementoKey(system);
    const rKey before = *key;
    const auto repair = sf4e::memento::RepairUnrecordableKey(*key);
    if (repair == sf4e::memento::KeyRepair::None) return;
    static unsigned noted = 0;
    if (noted >= 8) return;
    ++noted;
    spdlog::error("System memento: the battle's key names its object but no metadata (object={} count={} payload={} size={} next={} tracked={}); "
        "emptied so the record starts it again{}",
        before.mementoableObject, before.numMementos, before.mementos, before.sizeAllocated, before.nextMementoIndex,
        fKey::trackedKeys.count(key) != 0,
        repair == sf4e::memento::KeyRepair::ResetLeavingPayload ? ", leaving its payload behind" : "");
}

void fSystem::RecordAllToInternalMementos(rSystem* system, GameMementoKey::MementoID* id) {
    // This method exists entirely to work around the check that actors are
    // movable before the training mode mementos are saveable. This could be
    // replaced just by no-oping the `jz` instruction at 0x5d7fa0, but this
    // is probably more legible.
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    RepairSystemMementoKey(system);
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
