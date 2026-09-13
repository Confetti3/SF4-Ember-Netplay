#include <memory.h>
#include <windows.h>
#include <detours/detours.h>

#include "spdlog/spdlog.h"

#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__Hud.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__Hud.hxx"

namespace fHud = sf4e::Game::Battle::Hud;
namespace rHud = Dimps::Game::Battle::Hud;

using Dimps::Eva::Task;
using Dimps::Game::GameMementoKey;
using Dimps::Game::Sprite::Control;
using Dimps::Platform::dDeque_0x10;
using Dimps::Platform::GFxApp;
using Dimps::Platform::WithReleaser;

using fIUnit = sf4e::Game::Battle::IUnit;
using rIUnit = Dimps::Game::Battle::IUnit;

bool fHud::bAllowHudUpdate = true;

void fHud::Install() {
    Unit::Install();
    Announce::Unit::Install();
    Cockpit::Unit::Install();
    Cockpit::View::Install();
    Cursor::Unit::Install();
    Notice::Unit::Install();
    Result::Unit::Install();
    Subtitle::Unit::Install();
    Training::Unit::Install();

    void (fIUnit:: * _fSharedHudUpdate)(Task** task) = &fIUnit::SharedHudUpdate;

    DetourAttach(
        (PVOID*)&rIUnit::publicMethods.SharedHudUpdate,
        *(PVOID*)&_fSharedHudUpdate
    );
}

void fHud::Unit::Install() {
    DetourAttach((PVOID*)&rHud::Unit::staticMethods.Factory, Factory);
}


rHud::Unit* fHud::Unit::Factory(DWORD arg1, DWORD arg2) {
    rHud::Unit* u = rHud::Unit::staticMethods.Factory(arg1, arg2);
    // SF4's factory doesn't initialize the memory allocated to the
    // HUD unit to zero, resulting in the possibility of pointers
    // in the unit being non-zero. Null checks against those pointers
    // evaluate non-null, but don't actually point at real memory.
    //
    // This is particularly notable in the case of mementos, where
    // a null check on the training unit is executed before attempting
    // to save it. If the memory is not initialized, attempting to
    // access the training unit's vtable will segfault.
    rHud::Unit::ResetUnits(u);
    return u;
}

void fHud::Announce::Round::RecordToAdditionalMemento(rHud::Announce::Round* r, AdditionalMemento& m) {
    m.enabled = *(Control::GetEnabled(r));
    m.currentRound = *(Round::GetCurrentRound(r));

    // DO NOT save or restore either child sprite node- this will be
    // handled by the GFxApp's save state.
}

void fHud::Announce::Round::RestoreFromAdditionalMemento(rHud::Announce::Round* r, const AdditionalMemento& m) {
    bool currentlyEnabled = *(Control::GetEnabled(r));
    if (currentlyEnabled && !m.enabled) {
        (r->*Control::publicMethods.Disable_0x57bd80)();
    }
    else if (!currentlyEnabled && m.enabled) {
        (r->*Control::publicMethods.Enable_0x588450)();
    }
    *(Round::GetCurrentRound(r)) = m.currentRound;

    // DO NOT save or restore either child sprite node- this will be
    // handled by the GFxApp's save state.
}

void fHud::Announce::Unit::Install() {
    void (Unit:: * _fHudAnnounce_Update)(Task** task) = &HudAnnounce_Update;
    DetourAttach(
        (PVOID*)&rHud::Announce::Unit::publicMethods.HudAnnounce_Update,
        *(PVOID*)&_fHudAnnounce_Update
    );
}

void fHud::Announce::Unit::RecordToAdditionalMemento(rHud::Announce::Unit* u, AdditionalMemento& m) {
    rHud::Announce::View* v = *rHud::Announce::Unit::GetView(u);
    m.activeControl = *rHud::Announce::View::GetActiveControl(v);

    dDeque_0x10* queuedAnnouncements = rHud::Announce::View::GetQueuedAnnouncements(v);
    int i;
    dDeque_0x10::iterator iter;
    assert(queuedAnnouncements->size() <= 4);
    assert(sizeof(rHud::Announce::Announcement) == sizeof(dDeque_0x10::value_type));
    for (iter = queuedAnnouncements->begin(), i = 0; iter != queuedAnnouncements->end(); iter++, i++) {
        memcpy_s(&m.queuedAnnouncements[i], sizeof(rHud::Announce::Announcement), &*iter, sizeof(dDeque_0x10::value_type));
    }
    m.numQueuedAnnouncements = i;

    Round::RecordToAdditionalMemento(*rHud::Announce::View::GetRoundControl(v), m.round);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetFinalRoundControl(v), m.finalRound);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetExtraRoundControl(v), m.extraRound);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetFightControl(v), m.fight);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetKoControl(v), m.ko);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetDoubleKoControl(v), m.doubleKo);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetTimeControl(v), m.time);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetDrawControl(v), m.draw);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetPerfectControl(v), m.perfect);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetWinControl(v), m.win);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetLoseControl(v), m.lose);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetWins1Control(v), m.wins1);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetWins2Control(v), m.wins2);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetSuccessControl(v), m.success);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetClearControl(v), m.clear);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetReadyGoControl(v), m.readyGo);
    sf4e::Game::Sprite::SingleNodeControl::RecordToAdditionalMemento(*rHud::Announce::View::GetEditionControl(v), m.edition);
}

void fHud::Announce::Unit::RestoreFromAdditionalMemento(rHud::Announce::Unit* u, const AdditionalMemento& m) {
    rHud::Announce::View* v = *rHud::Announce::Unit::GetView(u);
    *rHud::Announce::View::GetActiveControl(v) = m.activeControl;

    dDeque_0x10* queuedAnnouncements = rHud::Announce::View::GetQueuedAnnouncements(v);
    (queuedAnnouncements->*dDeque_0x10::publicMethods.clear)();
    assert(sizeof(rHud::Announce::Announcement) == sizeof(dDeque_0x10::value_type));
    int i;
    for (i = 0; i < m.numQueuedAnnouncements; i++) {
        (queuedAnnouncements->*dDeque_0x10::publicMethods.push_back)((dDeque_0x10::value_type*)&m.queuedAnnouncements[i]);
    }

    Round::RestoreFromAdditionalMemento(*rHud::Announce::View::GetRoundControl(v), m.round);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetFinalRoundControl(v), m.finalRound);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetExtraRoundControl(v), m.extraRound);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetFightControl(v), m.fight);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetKoControl(v), m.ko);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetDoubleKoControl(v), m.doubleKo);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetTimeControl(v), m.time);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetDrawControl(v), m.draw);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetPerfectControl(v), m.perfect);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetWinControl(v), m.win);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetLoseControl(v), m.lose);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetWins1Control(v), m.wins1);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetWins2Control(v), m.wins2);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetSuccessControl(v), m.success);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetClearControl(v), m.clear);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetReadyGoControl(v), m.readyGo);
    sf4e::Game::Sprite::SingleNodeControl::RestoreFromAdditionalMemento(*rHud::Announce::View::GetEditionControl(v), m.edition);
}

void fHud::Announce::Unit::HudAnnounce_Update(Task** task) {
    if (bAllowHudUpdate) {
        (this->*rHud::Announce::Unit::publicMethods.HudAnnounce_Update)(task);
    }
}

void fHud::Cockpit::Unit::Install() {
    void (Unit:: * _fHudCockpit_Update)(Task** task) = &HudCockpit_Update;
    DetourAttach(
        (PVOID*)&rHud::Cockpit::Unit::publicMethods.HudCockpit_Update,
        *(PVOID*)&_fHudCockpit_Update
    );
}

void fHud::Cockpit::Unit::HudCockpit_Update(Task** task) {
    if (bAllowHudUpdate) {
        (this->*rHud::Cockpit::Unit::publicMethods.HudCockpit_Update)(task);
    }
}

void fHud::Cockpit::View::Install() {
    void (View::* _fRestoreFromInternalMementoKey)(GameMementoKey::MementoID* id) = &RestoreFromInternalMementoKey;
    DetourAttach(
        (PVOID*)&rHud::Cockpit::View::publicMethods.RestoreFromInternalMementoKey,
        *(PVOID*)&_fRestoreFromInternalMementoKey
    );
}

void fHud::Cockpit::View::RestoreFromInternalMementoKey(GameMementoKey::MementoID* id) {
    (this->*rHud::Cockpit::View::publicMethods.RestoreFromInternalMementoKey)(id);

    // SF4's implementation of the cockpit view's load operation is incomplete.
    //
    // When the cockpit view updates, it first delegates updates to its children,
    // then forcibly adjusts some of the children to take into account animations
    // like frame-out as well as to respect the user's HUD position settings.
    // The super-combo meter (Dimps::Game::Battle::Hud::Cockpit::Sc) stages
    // its child sprite nodes on update as necessary.
    // 
    // However, loading is a different story. When loading, the super-combo
    // meter will re-stage its child sprite nodes, but the View does _not_
    // adjust the position of the new sprite nodes. This isn't generally
    // user visible because when the save states are used in training mode,
    // the save and load operations occur very early in control flow, and
    // then the HUD update later in the control flow repositions the nodes
    // before rendering. Unfortunately, since our savestate-freeing method
    // requires loading the state, `ggpo_advance_frame()` will load state
    // _after_ the HUD update and _before_ rendering, which results in
    // sprites visibly out-of-place.
    // 
    // Work around this by updating the position of the child controls,
    // while taking care to not animate the sprites. It may be necessary
    // to skip updating the child controls as well, but specific controls
    // will need to be disabled or reenabled to prevent all the child
    // controls from being turned on at once (which could lead to scenarios
    // like win marks appearing in training mode).
    // 
    // One other possible workaround: never call `ggpo_advance_frame()` until
    // after rendering.
    //
    // If savestates ever move away from using SF4's internal memento keys,
    // this will need to be implemented a different way, although it may go
    // away entirely depending on the implementation of the load operation.
    GFxApp* scaleform = GFxApp::staticMethods.GetSingleton();
    float originalFrameCount = (scaleform->*GFxApp::publicMethods.GetFrameDelta_Float)();
    (scaleform->*GFxApp::publicMethods.SetFrameDelta_Float)(0.0);
    (this->*rHud::Cockpit::View::publicMethods.Update)();
    (scaleform->*GFxApp::publicMethods.SetFrameDelta_Float)(originalFrameCount);
}

void fHud::Cursor::Unit::Install() {
    void (Unit:: * _fHudCursor_Update)(Task** task) = &HudCursor_Update;
    DetourAttach(
        (PVOID*)&rHud::Cursor::Unit::publicMethods.HudCursor_Update,
        *(PVOID*)&_fHudCursor_Update
    );
}

void fHud::Cursor::Unit::HudCursor_Update(Task** task) {
    if (bAllowHudUpdate) {
        (this->*rHud::Cursor::Unit::publicMethods.HudCursor_Update)(task);
    }
}

void fHud::Notice::Bonus::RecordToAdditionalMemento(
    rHud::Notice::Bonus* b,
    AdditionalMemento& m
) {
    m.isEnabled = *Control::GetEnabled(b);
    m.activeSprite = *rHud::Notice::Bonus::GetActiveSprite(b);
    m.currentBonus = *rHud::Notice::Bonus::GetCurrentBonus(b);
    m.score = *rHud::Notice::Bonus::GetScore(b);
    m.isActive = *rHud::Notice::Bonus::GetIsActive(b);
}

void fHud::Notice::Bonus::RestoreFromAdditionalMemento(
    rHud::Notice::Bonus* b,
    const AdditionalMemento& m
) {
    *rHud::Notice::Bonus::GetActiveSprite(b) = m.activeSprite;
    *rHud::Notice::Bonus::GetCurrentBonus(b) = m.currentBonus;
    *rHud::Notice::Bonus::GetScore(b) = m.score;
    *rHud::Notice::Bonus::GetIsActive(b) = m.isActive;
    if (m.isEnabled) {
        (b->*rHud::Notice::Bonus::publicMethods.Enable)();
    } else {
        (b->*rHud::Notice::Bonus::publicMethods.Disable)();
    }
}

void fHud::Notice::Combo::RecordToAdditionalMemento(
    rHud::Notice::Combo* c,
    AdditionalMemento& m
) {
    m.isEnabled = *Control::GetEnabled(c);
    m.comboLength = *rHud::Notice::Combo::GetComboLength(c);
    m.score = *rHud::Notice::Combo::GetScore(c);
    m.isActive = *rHud::Notice::Combo::GetIsActive(c);
    m.shouldShowAdjective = *rHud::Notice::Combo::GetShouldShowAdjective(c);
}

void fHud::Notice::Combo::RestoreFromAdditionalMemento(
    rHud::Notice::Combo* c,
    const AdditionalMemento& m
) {
    *rHud::Notice::Combo::GetComboLength(c) = m.comboLength;
    *rHud::Notice::Combo::GetScore(c) = m.score;
    *rHud::Notice::Combo::GetIsActive(c) = m.isActive;
    *rHud::Notice::Combo::GetShouldShowAdjective(c) = m.shouldShowAdjective;
    if (m.isEnabled) {
        (c->*rHud::Notice::Combo::publicMethods.Enable)();
    } else {
        (c->*Control::publicMethods.Disable_0x57bd80)();
    }
}

void fHud::Notice::Player::RecordToAdditionalMemento(
    rHud::Notice::Player* p,
    AdditionalMemento& m
) {
    dDeque_0x10* queuedBonuses = rHud::Notice::Player::GetQueuedBonusNotices(p);
    dDeque_0x10* queuedCombos = rHud::Notice::Player::GetQueuedComboNotices(p);
    int i;
    dDeque_0x10::iterator iter;
    assert(queuedBonuses->size() <= 8);
    assert(queuedCombos->size() <= 8);
    assert(sizeof(rHud::Notice::NoticeData) == sizeof(dDeque_0x10::value_type));
    for (iter = queuedBonuses->begin(), i = 0; iter != queuedBonuses->end(); iter++, i++) {
        memcpy_s(&m.queuedBonuses[i], sizeof(rHud::Notice::NoticeData), &*iter, sizeof(dDeque_0x10::value_type));
    }
    m.nQueuedBonuses = i;
    for (iter = queuedCombos->begin(), i = 0; iter != queuedCombos->end(); iter++, i++) {
        memcpy_s(&m.queuedCombos[i], sizeof(rHud::Notice::NoticeData), &*iter, sizeof(dDeque_0x10::value_type));
    }
    m.nQueuedCombos = i;

    fHud::Notice::Combo::RecordToAdditionalMemento(
        rHud::Notice::Player::GetCombo(p)->obj,
        m.combo
    );
    WithReleaser<rHud::Notice::Bonus>* bonusCursor = rHud::Notice::Player::GetBonuses(p);
    for (int bonusIdx = 0; bonusIdx < 2; bonusIdx++) {
        fHud::Notice::Bonus::RecordToAdditionalMemento(
            bonusCursor[bonusIdx].obj,
            m.bonuses[bonusIdx]
        );
    }
}

void fHud::Notice::Player::RestoreFromAdditionalMemento(
    rHud::Notice::Player* p,
    const AdditionalMemento& m
) {
    dDeque_0x10* queuedBonuses = rHud::Notice::Player::GetQueuedBonusNotices(p);
    dDeque_0x10* queuedCombos = rHud::Notice::Player::GetQueuedComboNotices(p);
    (queuedBonuses->*dDeque_0x10::publicMethods.clear)();
    (queuedCombos->*dDeque_0x10::publicMethods.clear)();
    assert(sizeof(rHud::Notice::NoticeData) == sizeof(dDeque_0x10::value_type));
    int i;
    for (i = 0; i < m.nQueuedBonuses; i++) {
        (queuedBonuses->*dDeque_0x10::publicMethods.push_back)((dDeque_0x10::value_type*)&m.queuedBonuses[i]);
    }
    for (i = 0; i < m.nQueuedCombos; i++) {
        (queuedCombos->*dDeque_0x10::publicMethods.push_back)((dDeque_0x10::value_type*)&m.queuedCombos[i]);
    }

    fHud::Notice::Combo::RestoreFromAdditionalMemento(
        rHud::Notice::Player::GetCombo(p)->obj,
        m.combo
    );
    WithReleaser<rHud::Notice::Bonus>* bonusCursor = rHud::Notice::Player::GetBonuses(p);
    for (int bonusIdx = 0; bonusIdx < 2; bonusIdx++) {
        fHud::Notice::Bonus::RestoreFromAdditionalMemento(
            bonusCursor[bonusIdx].obj,
            m.bonuses[bonusIdx]
        );
    }
}

void fHud::Notice::Unit::Install() {
    void (Unit:: * _fRestoreFromInternalMementoKey)() = &RestoreFromInternalMementoKey;
    DetourAttach(
        (PVOID*)&rHud::Notice::Unit::publicMethods.RestoreFromInternalMementoKey,
        *(PVOID*)&_fRestoreFromInternalMementoKey
    );
}

void fHud::Notice::Unit::RestoreFromInternalMementoKey() {
    // SF4's existing Notice code doesn't actually restore from the key-
    // it uses the method as a way to clear any existing notices. If that
    // is the case for states used in rollback, all bonus and combo notices
    // would be completely hidden.
    return;
}

void fHud::Result::Unit::Install() {
    void (Unit:: * _fHudResult_Update)(Task** task) = &HudResult_Update;
    DetourAttach(
        (PVOID*)&rHud::Result::Unit::publicMethods.HudResult_Update,
        *(PVOID*)&_fHudResult_Update
    );
}

void fHud::Result::Unit::HudResult_Update(Task** task) {
    if (bAllowHudUpdate) {
        (this->*rHud::Result::Unit::publicMethods.HudResult_Update)(task);
    }
}

void fHud::Subtitle::Unit::Install() {
    void (Unit:: * _fHudSubtitle_Update)(Task** task) = &HudSubtitle_Update;
    DetourAttach(
        (PVOID*)&rHud::Subtitle::Unit::publicMethods.HudSubtitle_Update,
        *(PVOID*)&_fHudSubtitle_Update
    );
}

void fHud::Subtitle::Unit::HudSubtitle_Update(Task** task) {
    if (bAllowHudUpdate) {
        (this->*rHud::Subtitle::Unit::publicMethods.HudSubtitle_Update)(task);
    }
}

void fHud::Training::Unit::Install() {
    void (Unit:: * _fHudTraining_Update)(Task** task) = &HudTraining_Update;
    DetourAttach(
        (PVOID*)&rHud::Training::Unit::publicMethods.HudTraining_Update,
        *(PVOID*)&_fHudTraining_Update
    );
}

void fHud::Training::Unit::HudTraining_Update(Task** task) {
    if (bAllowHudUpdate) {
        (this->*rHud::Training::Unit::publicMethods.HudTraining_Update)(task);
    }
}
