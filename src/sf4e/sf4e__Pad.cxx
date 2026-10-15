#include <windows.h>
#include <detours/detours.h>

#include "../Dimps/Dimps__Pad.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__BackgroundPlay.hxx"
#include "sf4e__InputLag.hxx"
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
namespace {
std::atomic<bool> inputBlocked{false};
// The last update's input was read behind another window for Ember alone
// (BackgroundPlay::HidesInput), decided once where it was published: the
// caches are cleared and the getters give the game nothing.
std::atomic<bool> hiddenInput{false};
}
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
    // The focus period this poll samples under, and the call back from
    // Training offered as it begins, taken before any input is read: what the
    // training controls and go now are asked from this poll is accepted only
    // while both are still current (TrainingPad.hxx: TrainingControls).
    const auto focus=sf4e::Overlay::TrainingControls().Sample();
    auto& goNowGate=sf4e::input::TrainingGoNow();
    const auto offeredCall=goNowGate.Offered();
    const auto sharedSnapshot=sf4e::NetplayFacade::GetRuntimeSnapshotShared();
    const auto& snapshot=*sharedSnapshot;
    // Where Ember's menu may hold the pads follows where the game is now,
    // whatever the drawing thread last published (ui::MenuPads).
    auto& menuPads=sf4e::Overlay::MenuPads();
    menuPads.Place(snapshot.menuContext,sf4e::netplay::Fighting(snapshot.session.match));
    // A replay exported as a video, and a room's match this PC only watches,
    // run on behind another window whatever the player's setting says. Ember's
    // menu reads the pads there too, while it holds them and where Start opens
    // it, and so does go now while a call back from Training offers it.
    const bool watching=sf4e::Game::Battle::System::ggpo&&sf4e::UserApp::netplay&&sf4e::UserApp::netplay->spectating;
    const bool callOffered=snapshot.menuContext==sf4e::input::MenuContext::OfflineTraining&&offeredCall.Live();
    const auto policy=sf4e::BackgroundPlay::PolicyFor(snapshot.preferences.backgroundPlay,
        sf4e::replaystore::Exporting(),watching,menuPads.Takes()||callOffered);
    sf4e::BackgroundPlay::BeforePadUpdate(this,policy);
    sf4e::InputLag::BeforePadUpdate(snapshot.preferences.lowerKeyboardLag);
    (this->*rSystem::publicMethods.UpdateInputs)();
    // This is the native input publication boundary (00512180). Both
    // players' held/rising/falling/repeat caches are complete before any
    // event, including native pause, reads them. The provider stays intact.
    // Whether the game is given this input is decided here, with the window
    // as it stands now, and holds for the caches and the getters alike.
    const bool hidden=sf4e::BackgroundPlay::HidesFromGame(policy);
    hiddenInput=hidden;
    const auto& device=snapshot.inputDevice;
    unsigned mapped=0,physical=0;
    const bool connected=Dimps::Pad::ReadController(device.type,device.index,mapped,&physical);
    constexpr unsigned PhysicalStart=sf4e::input::NativeStart;
    const bool focused=focus.focused;
    // Start opens Ember at the main menu, and over a Practice match this PC
    // fights in, with the window in front or behind: the pads are polled
    // there for it. The request holds the pads at once, so the press is
    // cleared below before the game or the fight reads it.
    static bool mainArmed=false;
    static int ownerType=-1,ownerIndex=-1;
    if(!connected||!sf4e::input::ControllerOpensMenu(snapshot.menuContext)||snapshot.inputCapture!=sf4e::input::Capture::Idle||
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
    // Behind another window the gesture is read only while a call offers go
    // now, for go now: the reset, save and controls it asks for there are
    // refused without the focus (PostTrainingPad, TrainingControls::Accept).
    static sf4e::input::TrainingPadInput trainingPad;
    static bool trainingPadOwned=false;
    std::uint32_t ownedByGesture=0;
    const bool training=(focused||callOffered)&&connected&&snapshot.menuContext==sf4e::input::MenuContext::OfflineTraining&&
        snapshot.inputCapture==sf4e::input::Capture::Idle&&ownerType==device.type&&ownerIndex==device.index;
    if(!training) {
        trainingPad.Reset();
        if(trainingPadOwned) sf4e::Overlay::DropTrainingPad();
        trainingPadOwned=false;
    }
    else {
        trainingPadOwned=true;
        // The call back from a room, as its owner offered it when this poll
        // began; go now is the pad's only while nobody else has the press
        // (TrainingCallInput.hxx).
        const bool free=!sf4e::Overlay::CapturesMenuInput()&&!sf4e::battlePause.Paused();
        const auto call=!offeredCall.Live()?sf4e::input::TrainingCall::None:
            free&&device.type==sf4e::input::PadXInput?sf4e::input::TrainingCall::GoNow:sf4e::input::TrainingCall::Called;
        const auto place=sf4e::training::ReadPlace();
        sf4e::input::PadOwner owner;owner.generation=place.generation;owner.epoch=focus.epoch;
        const auto result=trainingPad.Update(physical,sf4e::Overlay::TrainingControls(),GetTickCount64()/1000.0,call,owner,place.x);
        const auto& events=result.events;
        ownedByGesture=events.owned;
        // View's go now is accepted, as the chord is, only under the focus
        // period its press was sampled in, and only for the call offered then.
        if(events.goNow) spdlog::info("Training: go now on the pad was {}{}",
            sf4e::Overlay::TrainingControls().PressGoNow(result.owner.epoch,offeredCall,goNowGate,sf4e::input::GoNowGate::Source::Pad,free)?"taken":"not taken",
            focused?"":" behind another window");
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
    const bool blocked=gate.Update(sf4e::Overlay::CapturesMenuInput()||hidden,held);
    inputBlocked=blocked;
    // Native Start is untouched while Ember is closed; an open overlay,
    // including the training controls, captures all inputs until release,
    // and so do pads polled behind another window for Ember's menu alone.
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
    if (sf4e::Overlay::CapturesMenuInput() || hiddenInput.load()) { captured = buttons; return 0; }
    // Release each captured button before returning it to the native menu.
    captured &= buttons;
    return buttons & ~captured;
}

unsigned int fSystem::GetButtons_MappedOn(int pindex) { return ReadButtons(pindex, false); }

unsigned int fSystem::GetButtons_RawOn(int pindex) { return ReadButtons(pindex, true); }
