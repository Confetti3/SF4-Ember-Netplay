#include <windows.h>
#include <detours/detours.h>

#include "spdlog/spdlog.h"

#include "../Dimps/Dimps__Game__Battle__Vfx.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../common/VfxObjectMemento.hxx"
#include "sf4e__Game__Battle__Vfx.hxx"

using Dimps::Platform::list_entry;
using Dimps::Platform::list;

namespace fVfx = sf4e::Game::Battle::Vfx;
namespace rVfx = Dimps::Game::Battle::Vfx;

using fColorFade = fVfx::ColorFade;
using rColorFade = rVfx::ColorFade;

int fColorFade::HIGHEST_OBSERVED_FADES = 0;

namespace {
// See common/VfxObjectMemento.hxx: the native object memento copies one dword
// past each object, onto the next heap header for the last one in an array.
void ClampObjectMementoCopy(const char* side, BYTE* instruction) {
    namespace memento = sf4e::vfx_memento;
    const SIZE_T length = 5;
    DWORD old = 0;
    if (!VirtualProtect(instruction, length, PAGE_EXECUTE_READWRITE, &old)) {
        spdlog::warn("VFX: object memento {} copy not clamped (VirtualProtect error {})", side, GetLastError());
        return;
    }
    const memento::Patch patch = memento::ClampCopyCount(instruction);
    VirtualProtect(instruction, length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), instruction, length);
    if (patch == memento::Patch::Unexpected) {
        spdlog::warn("VFX: object memento {} copy not clamped ({})", side, memento::Describe(patch));
    } else {
        spdlog::info("VFX: object memento {} copy clamped to the object ({})", side, memento::Describe(patch));
    }
}
}

void fVfx::Install() {
    fColorFade::Install();
    ClampObjectMementoCopy("record", rVfx::Object::recordCopyCount);
    ClampObjectMementoCopy("restore", rVfx::Object::restoreCopyCount);
}

void fColorFade::Install() {
    list_entry<rVfx::ColorFadeData>* (fColorFade::* _fSpawn)(void* sourceData) = &Spawn;
    DetourAttach((PVOID*)&rColorFade::publicMethods.Spawn, *(PVOID*)&_fSpawn);
}

list_entry<rVfx::ColorFadeData>* fColorFade::Spawn(void* sourceData) {
    rColorFade* _this = (rColorFade*)this;
    list_entry<rVfx::ColorFadeData>* out = (_this->*rColorFade::publicMethods.Spawn)(sourceData);
    list<rVfx::ColorFadeData>* fadeList = rColorFade::GetList(_this);
    if (fadeList->numUsed > HIGHEST_OBSERVED_FADES) {
        spdlog::info("ColorFade: new max list size updated from {} to {}", HIGHEST_OBSERVED_FADES, fadeList->numUsed);
        HIGHEST_OBSERVED_FADES = fadeList->numUsed;
    }
    return out;
}
