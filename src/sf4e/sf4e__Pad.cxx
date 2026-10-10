#include <windows.h>
#include <detours/detours.h>

#include "../Dimps/Dimps__Pad.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__BackgroundPlay.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__ReplayStore.hxx"
#include "sf4e__UserApp.hxx"
#include "../training/TrainingRuntime.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "../common/TrainingPad.hxx"
#include "../common/TrainingCallInput.hxx"
#include "../common/BattlePause.hxx"
#include "../common/MenuInputCapture.hxx"
#include <spdlog/spdlog.h>
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
    // The focus period this poll samples under, taken before any input is
    // read: what the training controls are asked from this poll is accepted
    // only while it is still current (TrainingPad.hxx: TrainingControls).
    const auto focus=sf4e::Overlay::TrainingControls().Sample();
    const auto sharedSnapshot=sf4e::NetplayFacade::GetRuntimeSnapshotShared();
    const auto& snapshot=*sharedSnapshot;
    // A replay exported as a video, and a room's match this PC only watches,
    // run on behind another window whatever the player's setting says.
    const bool watching=sf4e::Game::Battle::System::ggpo&&sf4e::UserApp::netplay&&sf4e::UserApp::netplay->spectating;
    sf4e::BackgroundPlay::BeforePadUpdate(this,sf4e::BackgroundPlay::PolicyFor(snapshot.preferences.backgroundPlay,
        sf4e::replaystore::Exporting(),watching));
    (this->*rSystem::publicMethods.UpdateInputs)();
    // This is the native input publication boundary (00512180). Both
    // players' held/rising/falling/repeat caches are complete before any
    // event, including native pause, reads them. The provider stays intact.
    const auto& device=snapshot.inputDevice;
    unsigned mapped=0,physical=0;
    const bool connected=Dimps::Pad::ReadController(device.type,device.index,mapped,&physical);
    constexpr unsigned PhysicalStart=sf4e::input::NativeStart;
    const bool focused=focus.focused;
    static bool mainArmed=false;
    static int ownerType=-1,ownerIndex=-1;
    if(!focused||!connected||!snapshot.atMainMenu||snapshot.inputCapture!=sf4e::input::Capture::Idle||
        ownerType!=device.type||ownerIndex!=device.index) mainArmed=false;
    else if(!physical)mainArmed=true;
    else if(mainArmed&&(physical&PhysicalStart)&&!sf4e::Overlay::CapturesMenuInput()) {
        mainArmed=false;sf4e::Overlay::RequestMainControls();
    }
    // Offline Training: the pad's Back and Start (TrainingPad.hxx). Each press
    // keeps the battle and pad owner it went down under, and the position
    // events it makes are posted in order for the drawing thread with them.
    // The chord opens or closes the controls in their controller at once, and
    // the capture follows that controller, so the caches cleared below hide
    // its Start from native pause in this same frame; a chord that opens
    // nothing (under the call) still owns its Start, cleared there as well.
    static sf4e::input::TrainingPadInput trainingPad;
    static bool trainingPadOwned=false;
    std::uint32_t ownedByGesture=0;
    const bool training=focused&&connected&&snapshot.menuContext==sf4e::input::MenuContext::OfflineTraining&&
        snapshot.inputCapture==sf4e::input::Capture::Idle&&ownerType==device.type&&ownerIndex==device.index;
    if(!training) {
        trainingPad.Reset();
        if(trainingPadOwned) sf4e::Overlay::DropTrainingPad();
        trainingPadOwned=false;
    }
    else {
        trainingPadOwned=true;
        // The call back from a room, as its owner offers it now; go now is
        // the pad's only while nobody else has the press (TrainingCallInput.hxx).
        auto& goNow=sf4e::input::TrainingGoNow();
        const bool free=!sf4e::Overlay::CapturesMenuInput()&&!sf4e::battlePause.Paused();
        const auto call=!goNow.Offered().Live()?sf4e::input::TrainingCall::None:
            free&&device.type==sf4e::input::PadXInput?sf4e::input::TrainingCall::GoNow:sf4e::input::TrainingCall::Called;
        const auto place=sf4e::training::ReadPlace();
        sf4e::input::PadOwner owner;owner.generation=place.generation;owner.epoch=focus.epoch;
        const auto result=trainingPad.Update(physical,sf4e::Overlay::TrainingControls(),GetTickCount64()/1000.0,call,owner,place.x);
        const auto& events=result.events;
        ownedByGesture=events.owned;
        if(events.goNow&&!goNow.Press(sf4e::input::GoNowGate::Source::Pad,free)) spdlog::info("Training: go now on the pad was not taken");
        using Kind=sf4e::input::TrainingPadEvent::Kind;
        for(const Kind kind:{Kind::Reset,Kind::Save}) {
            if(kind==Kind::Reset?!events.reset:!events.save) continue;
            sf4e::input::TrainingPadEvent event;
            event.kind=kind;event.generation=result.owner.generation;event.epoch=result.owner.epoch;
            event.place[0]=result.place[0];event.place[1]=result.place[1];
            sf4e::Overlay::PostTrainingPad(event);
        }
    }
    ownerType=device.type;ownerIndex=device.index;
    const unsigned held=sf4e::input::NativeMenuHeld(this);
    static sf4e::input::MenuInputCapture gate;
    const bool blocked=gate.Update(sf4e::Overlay::CapturesMenuInput(),held);
    inputBlocked=blocked;
    // Native Start is untouched while Ember is closed; an open overlay,
    // including the training controls, captures all inputs until release.
    if(blocked)sf4e::input::ClearNativeMenuInputs(this);
    else if(ownedByGesture&sf4e::input::PhysicalStart)sf4e::input::ClearNativeMenuInputs(this,~sf4e::input::NativeStart);
}

unsigned int fSystem::ReadButtons(int pindex, bool raw) {
    if (playbackFrame > -1 && pindex >= 0 && pindex < 2) {
        const Inputs& played = playbackData[playbackFrame][pindex];
        return raw ? played.rawOn : played.mappedOn;
    }
    sf4e::training::Input trainingInput;
    if (sf4e::training::ReadOverride(pindex, trainingInput)) return raw ? trainingInput.raw : trainingInput.mapped;

    const auto buttons = raw
        ? (this->*rSystem::publicMethods.GetButtons_RawOn)(pindex)
        : (this->*rSystem::publicMethods.GetButtons_MappedOn)(pindex);
    // Buttons captured by the menu, per channel and player.
    static unsigned int held[2][2]{};
    if (pindex < 0 || pindex >= 2) return buttons;
    unsigned int& captured = held[raw ? 1 : 0][pindex];
    if (sf4e::Overlay::CapturesMenuInput()) { captured = buttons; return 0; }
    // Release each captured button before returning it to the native menu.
    captured &= buttons;
    return buttons & ~captured;
}

unsigned int fSystem::GetButtons_MappedOn(int pindex) { return ReadButtons(pindex, false); }

unsigned int fSystem::GetButtons_RawOn(int pindex) { return ReadButtons(pindex, true); }
