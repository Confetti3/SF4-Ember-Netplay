#include <windows.h>
#include <detours/detours.h>

#include "spdlog/spdlog.h"

#include "../Dimps/Dimps__Game__Battle__Effect.hxx"
#include "sf4e__Game__Battle__Effect.hxx"

namespace fEffect = sf4e::Game::Battle::Effect;
namespace rEffect = Dimps::Game::Battle::Effect;
using fActorManager = fEffect::ActorManager;
using rActorManager = rEffect::ActorManager;

void fEffect::Install() {
    fActorManager::Install();
}

void fActorManager::Install() {
    size_t(fActorManager:: * _fGetMementoSize)() = &GetMementoSize;
    DetourAttach((PVOID*)&rActorManager::publicMethods.GetMementoSize, *(PVOID*)&_fGetMementoSize);
}

size_t fActorManager::GetMementoSize() {
    rActorManager* _this = (rActorManager*)this;
    int expectedMaximum = (0xc0 + 5 * (0x110 * 0x40));
    int observed = (this->*rActorManager::publicMethods.GetMementoSize)();
    if (observed > expectedMaximum) {
        spdlog::warn("Actor manager memento undersized! Expected {}, got {}", expectedMaximum, observed);
        return observed;
    }
    return expectedMaximum;
}
