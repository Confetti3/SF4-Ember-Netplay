#include <windows.h>
#include <detours/detours.h>

#include "../Dimps/Dimps__Pad.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__Overlay.hxx"
#include "../training/TrainingRuntime.hxx"
#include "sf4e__NetplayFacade.hxx"
#include <atomic>

namespace rPad = Dimps::Pad;
using rSystem = rPad::System;

namespace fPad = sf4e::Pad;
using fSystem = fPad::System;

fSystem::Inputs fSystem::playbackData[PLAYBACK_MAX][2];
int fSystem::playbackFrame = -1;
namespace { std::atomic<bool> inputBlocked{false}; }
bool fPad::MenuInputBlocked() { return inputBlocked.load(); }

void fPad::Install() {
	System::Install();
}

void fSystem::Install() {
    void (fSystem::* update)() = &UpdateInputs;
    DetourAttach((PVOID*)&rSystem::publicMethods.UpdateInputs, *(PVOID*)&update);
    unsigned int (fSystem:: * _fGetButtons_MappedOn)(int) = &GetButtons_MappedOn;
    unsigned int (fSystem:: * _fGetButtons_RawOn)(int) = &GetButtons_RawOn;
    DetourAttach((PVOID*)&rSystem::publicMethods.GetButtons_MappedOn, *(PVOID*)&_fGetButtons_MappedOn);
    DetourAttach((PVOID*)&rSystem::publicMethods.GetButtons_RawOn, *(PVOID*)&_fGetButtons_RawOn);
}

void fSystem::UpdateInputs() {
    (this->*rSystem::publicMethods.UpdateInputs)();
    // This is the native input publication boundary (00512180). Both
    // players' held/rising/falling/repeat caches are complete before any
    // event, including native pause, reads them. The provider stays intact.
    const auto snapshot=sf4e::NetplayFacade::GetRuntimeSnapshot();
    const auto& device=snapshot.inputDevice;
    unsigned mapped=0,physical=0;
    const bool connected=Dimps::Pad::ReadController(device.type,device.index,mapped,&physical);
    constexpr unsigned PhysicalStart=sf4e::input::NativeStart;
    const bool focused=sf4e::Overlay::HasInputFocus();
    static bool mainArmed=false;
    static int ownerType=-1,ownerIndex=-1;
    if(!focused||!connected||!snapshot.atMainMenu||snapshot.inputCapture!=sf4e::input::Capture::Idle||
        ownerType!=device.type||ownerIndex!=device.index) mainArmed=false;
    else if(!physical)mainArmed=true;
    else if(mainArmed&&(physical&PhysicalStart)&&!sf4e::Overlay::CapturesMenuInput()) {
        mainArmed=false;sf4e::Overlay::RequestMainControls();
    }
    ownerType=device.type;ownerIndex=device.index;
    const unsigned held=sf4e::input::NativeMenuHeld(this);
    static sf4e::input::MenuInputCapture gate;
    const bool blocked=gate.Update(sf4e::Overlay::CapturesMenuInput(),held);
    inputBlocked=blocked;
    // Training is keyboard-only. Native Start is untouched while Ember is
    // closed; an open overlay captures all inputs until release.
    if(blocked)sf4e::input::ClearNativeMenuInputs(this);
}

unsigned int fSystem::GetButtons_MappedOn(int pindex) {
    if (playbackFrame > -1 && pindex >= 0 && pindex < 2) {
        return playbackData[playbackFrame][pindex].mappedOn;
    }
    sf4e::training::Input trainingInput;
    if (sf4e::training::ReadOverride(pindex, trainingInput)) return trainingInput.mapped;

    rSystem* _this = (rSystem*)this;
    const auto buttons = (this->*rSystem::publicMethods.GetButtons_MappedOn)(pindex);
    static unsigned int held[2]{};
    if (pindex < 0 || pindex >= 2) return buttons;
    if (sf4e::Overlay::CapturesMenuInput()) { held[pindex] = buttons; return 0; }
    // Release each captured button before returning it to the native menu.
    held[pindex] &= buttons;
    return buttons & ~held[pindex];
}

unsigned int fSystem::GetButtons_RawOn(int pindex) {
    if (playbackFrame > -1 && pindex >= 0 && pindex < 2) {
        return playbackData[playbackFrame][pindex].rawOn;
    }
    sf4e::training::Input trainingInput;
    if (sf4e::training::ReadOverride(pindex, trainingInput)) return trainingInput.raw;

    rSystem* _this = (rSystem*)this;
    const auto buttons = (this->*rSystem::publicMethods.GetButtons_RawOn)(pindex);
    static unsigned int held[2]{};
    if (pindex < 0 || pindex >= 2) return buttons;
    if (sf4e::Overlay::CapturesMenuInput()) { held[pindex] = buttons; return 0; }
    held[pindex] &= buttons;
    return buttons & ~held[pindex];
}
