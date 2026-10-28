#include <set>

#include <windows.h>
#include <detours/detours.h>
#include "spdlog/spdlog.h"

#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__ReplayStore.hxx"
#include "sf4e__ReplayPlayback.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__MementoGuards.hxx"

namespace rGame = Dimps::Game;
using rSpriteNode = Dimps::Eva::IEmSpriteNode;
using rKey = rGame::GameMementoKey;
using rControl = rGame::Sprite::Control;
using rSingleNodeControl = rGame::Sprite::SingleNodeControl;

namespace fGame = sf4e::Game;
using fKey = fGame::GameMementoKey;
using fSingleNodeControl = fGame::Sprite::SingleNodeControl;

std::set<rKey*> fKey::trackedKeys;
bool fGame::MementoFailure::record = false;
bool fGame::MementoFailure::restore = false;

namespace {
    // The key whose fKey::Initialize is in its native call. Native Initialize
    // clears its own key first, and that call lands in the ClearKey detour.
    // Erasing the key there only for Initialize to insert it again frees and
    // allocates a set node per key per save. The skip is keyed on this one
    // key, never on a nesting depth, so any other key cleared during an
    // Initialize is still erased.
    rKey* initializingKey = nullptr;

    struct InitializingKeyScope {
        rKey* previous;
        explicit InitializingKeyScope(rKey* key) : previous(initializingKey) { initializingKey = key; }
        ~InitializingKeyScope() { initializingKey = previous; }
        InitializingKeyScope(const InitializingKeyScope&) = delete;
        InitializingKeyScope& operator=(const InitializingKeyScope&) = delete;
    };
}

void fGame::Install() {
    Battle::Install();
    GameMementoKey::Install();
    sf4e::replaystore::Install();
    sf4e::replayplayback::Install();
}

void fKey::Install() {
    void (fKey::* _fInitialize)(void*, int) = &Initialize;
    void (fKey:: * _fClearKey)() = &ClearKey;
    DetourAttach((PVOID*)&rKey::publicMethods.Initialize, *(PVOID*)&_fInitialize);
    DetourAttach((PVOID*)&rKey::publicMethods.ClearKey, *(PVOID*)&_fClearKey);
}

void fKey::Initialize(void* mementoable, int numMementos) {
    // Initialize clears its previous payload internally. Check here too so
    // ownership enforcement does not depend on that native call's routing.
    sf4e::memento::NoteEngineClear(this, "initialize");
    {
        InitializingKeyScope initializing(this);
        (this->*rKey::publicMethods.Initialize)(mementoable, numMementos);
    }
    trackedKeys.insert(this);
}

void fKey::ClearKey() {
    sf4e::memento::NoteEngineClear(this, "clear");
    (this->*rKey::publicMethods.ClearKey)();
    // Initialize re-inserts this key as soon as its native call returns, and
    // nothing reads trackedKeys inside that call, so membership afterwards is
    // the same either way.
    if (static_cast<rKey*>(this) != initializingKey) trackedKeys.erase((rKey*)this);
}

void fSingleNodeControl::RecordToAdditionalMemento(rSingleNodeControl* c, AdditionalMemento& m) {
    Dimps::Eva::IEmSpriteNode* n = *rSingleNodeControl::GetSpriteNode(c);
    m.enabled = *(rControl::GetEnabled(c));
    m.currentFrame = *rSingleNodeControl::GetCurrentFrame(c);

    // DO NOT save or restore the sprite node- this will be handled by
    // the GFxApp's save state.
}

void fSingleNodeControl::RestoreFromAdditionalMemento(rSingleNodeControl* c, const AdditionalMemento& m) {
    Dimps::Eva::IEmSpriteNode* n = *rSingleNodeControl::GetSpriteNode(c);

    bool currentlyEnabled = *(rControl::GetEnabled(c));
    if (currentlyEnabled && !m.enabled) {
        (c->*rControl::publicMethods.Disable_0x57bd80)();
    } else if (!currentlyEnabled && m.enabled) {
        (c->*rControl::publicMethods.Enable_0x577910)();
    }
    *rSingleNodeControl::GetCurrentFrame(c) = m.currentFrame;

    // DO NOT save or restore the sprite node- this will be handled by
    // the GFxApp's save state.
}
