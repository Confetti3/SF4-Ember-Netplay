#include <set>

#include <windows.h>
#include <detours/detours.h>
#include "spdlog/spdlog.h"

#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__Game__Battle.hxx"

namespace rGame = Dimps::Game;
using rSpriteNode = Dimps::Eva::IEmSpriteNode;
using rKey = rGame::GameMementoKey;
using rControl = rGame::Sprite::Control;
using rSingleNodeControl = rGame::Sprite::SingleNodeControl;

namespace fGame = sf4e::Game;
using fKey = fGame::GameMementoKey;
using fSingleNodeControl = fGame::Sprite::SingleNodeControl;

std::set<rKey*> fKey::trackedKeys;

void fGame::Install() {
    Battle::Install();
    GameMementoKey::Install();
}

void fKey::Install() {
    void (fKey::* _fInitialize)(void*, int) = &Initialize;
    void (fKey:: * _fClearKey)() = &ClearKey;
    DetourAttach((PVOID*)&rKey::publicMethods.Initialize, *(PVOID*)&_fInitialize);
    DetourAttach((PVOID*)&rKey::publicMethods.ClearKey, *(PVOID*)&_fClearKey);
}

void fKey::Initialize(void* mementoable, int numMementos) {
    (this->*rKey::publicMethods.Initialize)(mementoable, numMementos);
    trackedKeys.insert(this);
}

void fKey::ClearKey() {
    (this->*rKey::publicMethods.ClearKey)();
    trackedKeys.erase((rKey*)this);
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
