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
#include "sf4e__GameThemes.hxx"
#include "../common/HudPictureGuard.hxx"

#include <cstring>
#include <string>

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

// The native queues have no fixed bound; the memento does. A queue longer than
// its slots, or a stored count outside them, fails the save or the load
// (Game::MementoFailure) rather than rolling back to a HUD the game never had.
template <typename Queue> static void CaptureQueue(Queue& memento, dDeque_0x10* native, const char* name) {
    if (memento.Capture(native->begin(), native->end())) return;
    spdlog::error("HUD memento: the {} queue holds {} entries, more than its {} slots", name, native->size(), Queue::Capacity);
    sf4e::Game::MementoFailure::record = true;
}

template <typename Queue> static void RestoreQueue(const Queue& memento, dDeque_0x10* native) {
    if (!memento.Valid()) {
        spdlog::error("HUD memento: a stored queue count of {} is outside its {} slots", memento.count, Queue::Capacity);
        sf4e::Game::MementoFailure::restore = true;
        return;
    }
    (native->*dDeque_0x10::publicMethods.clear)();
    for (const auto& entry : memento)
        (native->*dDeque_0x10::publicMethods.push_back)((dDeque_0x10::value_type*)&entry);
}

namespace {
using Dimps::Game::Sprite::EmSprite;
using Dimps::Game::Sprite::EmbTextureList;
using Dimps::Game::Sprite::PictureKey;

// The first bytes of EmSprite::ApplyTextureKey (0x6ED070) in this game build.
constexpr unsigned char kApplyTextureKeyNative[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x53, 0x8B, 0x5D, 0x08, 0x8B, 0x03, 0x56, 0x57, 0x8B, 0xF1 };
// Game thread only: the HUD's sprites are updated there.
sf4e::hud_picture::Skips pictureSkips;
// Whether the picture read is guarded: its bytes were this game build's and the detour attached.
bool pictureGuardInstalled = false;

std::string ActionName(EmSprite* sprite) {
    const char* action = *EmSprite::GetAction(sprite);
    if (!action) return "?";
    std::string name(action + 0x30, strnlen(action + 0x30, 0x20));
    for (char& c : name) if (c < 0x20 || c > 0x7e) c = '?';
    return name;
}

// A picture the sprite's sheet does not have is drawn as the game draws a sprite with no picture
// (a negative number): the sprite's texture is cleared. The HUD only: no game state reads it.
char __fastcall ApplyTextureKey(EmSprite* sprite, void*, PictureKey* key, void* colour) {
    const EmbTextureList* list = *EmSprite::GetTextureList(sprite);
    if (key && sf4e::hud_picture::Missing(list != nullptr, list ? list->items : nullptr, list ? list->count : 0, key->picture)) {
        if (pictureSkips.Note())
            spdlog::warn("HUD: {} names picture {} and its sheet has {}; it is drawn without a picture (logged once a battle)",
                ActionName(sprite), key->picture, list->count);
        PictureKey none = *key;
        none.picture = -1;
        return (sprite->*EmSprite::publicMethods.ApplyTextureKey)(&none, colour);
    }
    return (sprite->*EmSprite::publicMethods.ApplyTextureKey)(key, colour);
}

void InstallPictureGuard() {
    const void* at = *(PVOID*)&EmSprite::publicMethods.ApplyTextureKey;
    if (std::memcmp(at, kApplyTextureKeyNative, sizeof kApplyTextureKeyNative) != 0) {
        spdlog::warn("HUD: the sprite picture read holds other bytes than this game build's; pictures a sheet lacks are not guarded");
        return;
    }
    pictureGuardInstalled = DetourAttach((PVOID*)&EmSprite::publicMethods.ApplyTextureKey, (PVOID)ApplyTextureKey) == NO_ERROR;
}
}

bool fHud::PictureGuardInstalled() { return pictureGuardInstalled; }

void fHud::Install() {
    InstallPictureGuard();
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
    // A battle's HUD units are made before any of its sprites takes a picture.
    pictureSkips.Reset();
    sf4e::GameThemes::BattleHudMade();
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
    CaptureQueue(m.queuedAnnouncements, rHud::Announce::View::GetQueuedAnnouncements(v), "announcement");

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
    RestoreQueue(m.queuedAnnouncements, rHud::Announce::View::GetQueuedAnnouncements(v));

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
    CaptureQueue(m.queuedBonuses, rHud::Notice::Player::GetQueuedBonusNotices(p), "bonus notice");
    CaptureQueue(m.queuedCombos, rHud::Notice::Player::GetQueuedComboNotices(p), "combo notice");

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
    RestoreQueue(m.queuedBonuses, rHud::Notice::Player::GetQueuedBonusNotices(p));
    RestoreQueue(m.queuedCombos, rHud::Notice::Player::GetQueuedComboNotices(p));

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
