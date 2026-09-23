#include "TrainingRuntime.hxx"
#include "TrainingCapture.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../sf4e/sf4e__Game__Battle__System.hxx"
#include "../sf4e/sf4e__Overlay.hxx"
#include "../sf4e/sf4e__Pad.hxx"
#include <mutex>
#include <spdlog/spdlog.h>

namespace sf4e { namespace training {
namespace {
using Native = Dimps::Game::Battle::System;
using Battle = sf4e::Game::Battle::System;
using Pad = Dimps::Pad::System;
Session session;
FrameMeter meter;
// Normal shutdown owns this worker. Never join a thread from a DLL destructor
// while Windows holds the loader lock.
TrainingCapture* capture = nullptr;
std::mutex mutex;
View published;
std::deque<Command> commands;
// Separate ownership from the GGPO pool and the developer save slot.
Battle::SaveState checkpoint;
Frame output;
thread_local bool overriding = false;
bool sampling = false;
bool commitInput = false;
int beforeFrame = 0;
std::uint64_t commandId=0;
bool commandAccepted=false;
// Updates that advanced more than one frame, each of which resets the frame
// meter. Logged per battle: a large count explains a missing frame-advantage
// readout (ledger F-006).
std::uint64_t gapResets = 0;
}
View ReadView() { std::lock_guard<std::mutex> lock(mutex); return published; }
bool ControlsAvailable() {
    if(!session.GetView().available || Battle::ggpo) return false;
    auto* system=Native::staticMethods.GetSingleton();
    return system && (system->*Native::publicMethods.GetGameMode)()==Dimps::Game::Battle::GAMEMODE_TRAINING &&
        !(system->*Native::publicMethods.IsLeavingBattle)();
}
bool Submit(Command command) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!published.available || command.generation != published.generation || commands.size() >= 32) return false;
    commands.push_back(command); return true;
}
bool ReadOverride(int side, Input& result) {
    if (!overriding || side < 0 || side >= 2) return false;
    result = output[side]; return true;
}
void BeforeUpdate(Native* system, bool networkOwned) {
    overriding = false; sampling = false; commitInput = false;
    const bool available = !networkOwned &&
        (system->*Native::publicMethods.GetGameMode)() == Dimps::Game::Battle::GAMEMODE_TRAINING &&
        !(system->*Native::publicMethods.IsLeavingBattle)();
    if (!available) {
        if (session.GetView().available) CloseBattle();
        return;
    }
    if (!session.GetView().available) session.Enter();
    session.SetReady((system->*Native::publicMethods.IsFight)() &&
        *Native::GetReadyState(system) == Native::RS_FIGHT);
    std::deque<Command> pending;
    { std::lock_guard<std::mutex> lock(mutex); pending.swap(commands); }
    for (const auto& command : pending) {
        commandId=command.requestId; commandAccepted=false;
        if (command.generation == session.GetView().generation && command.action == Action::AutoFreeze) {
            meter.SetAutoFreeze(command.value != 0); commandAccepted=true; continue;
        }
        if (!session.Apply(command)) continue;
        commandAccepted=true;
        if (command.action == Action::Save) {
            if (checkpoint.used) Battle::SaveState::Free(&checkpoint);
            // A state the memento cannot represent is refused, not kept
            // without its task functors (ledger A-001).
            commandAccepted = Battle::SaveState::Save(&checkpoint);
            session.SetCheckpoint(checkpoint.used);
        } else if (command.action == Action::Restore) {
            commandAccepted = Battle::SaveState::Load(&checkpoint);
            meter.Reset();
            if (!commandAccepted) {
                // A partly restored engine cannot be played on; leave the battle.
                spdlog::error("Training: the checkpoint did not fully restore; leaving the battle");
                *Native::GetReadyState(system) = Native::RS_ISLEAVING;
            }
        } else if (command.action == Action::ClearHistory) {
            meter.Reset();
        }
    }
    // Opening either overlay suspends playback and recording; native pause
    // frames are also excluded by the before/after simulation counter check.
    if (!session.GetView().ready) return;
    beforeFrame = Native::GetNumFramesSimulated_FixedPoint(system)->integral;
    sampling = true;
    if (sf4e::Overlay::CapturesMenuInput() || sf4e::Pad::MenuInputBlocked()) return;
    Pad* pad = Pad::staticMethods.GetSingleton();
    Frame physical;
    for (int side = 0; side < 2; ++side) {
        physical[side].mapped = (pad->*Pad::publicMethods.GetButtons_MappedOn)(side);
        physical[side].raw = (pad->*Pad::publicMethods.GetButtons_RawOn)(side);
    }
    output = session.Prepare(physical);
    commitInput = true;
    overriding = session.GetView().mode != Mode::Idle;
}
void AfterUpdate(Native* system) {
    overriding = false;
    // The native integral field wraps at 16 bits. Count only one accepted
    // step; native resets/time jumps must not become recorded input frames.
    const int afterFrame = sampling ? Native::GetNumFramesSimulated_FixedPoint(system)->integral : beforeFrame;
    const auto delta = static_cast<std::uint16_t>(afterFrame - beforeFrame);
    if (sampling && delta == 1) {
        if (commitInput) session.Commit(output);
        using Actor = Dimps::Game::Battle::Chara::Actor;
        using Unit = Dimps::Game::Battle::Chara::Unit;
        Unit* unit = (system->*Native::publicMethods.GetCharaUnit)();
        std::array<FighterSample, 2> fighters;
        for (unsigned side = 0; side < 2 && unit; ++side) {
            Actor* actor = (unit->*Unit::publicMethods.GetActorByIndex)(side);
            if (!actor) continue;
            auto& sample = fighters[side];
            sample.status = (actor->*Actor::publicMethods.GetStatus)();
            sample.action = (actor->*Actor::publicMethods.GetActionID)();
            sample.posture = (actor->*Actor::publicMethods.GetActionPosture)();
            sample.basicActionInhibited = (actor->*Actor::publicMethods.GetBasicActionInhibit)() != 0;
            Dimps::Math::FixedPoint value{};
            (system->*Native::publicMethods.GetUnitTimeScale_Fixed)(&value, side);
            sample.timeScale = Dimps::Math::FixedToFloat(&value);
            (actor->*Actor::publicMethods.GetActionFrame)(&value);
            sample.actionFrame = Dimps::Math::FixedToFloat(&value);
            if (sample.status == Actor::AS_SKILL && sample.action >= 0) {
                const auto* script = (actor->*Actor::publicMethods.GetActionScript)(sample.action);
                // Native BAC action header: first/last attack boundary,
                // interruptible frame, total animation frames. Zero/zero is
                // common on recovery-only actions and is not 0f startup.
                if (script && script[1] > script[0] && script[0] < script[3] && script[3] <= 4096) {
                    sample.firstActiveFrame = script[0];
                    sample.lastActiveFrame = script[1];
                    sample.boundaryProvenance = BoundaryProvenance::BacActionHeader;
                }
            }
            (actor->*Actor::publicMethods.GetDamage)(&value);
            sample.damage = Dimps::Math::FixedToFloat(&value);
            (actor->*Actor::publicMethods.GetComboDamage)(&value);
            sample.comboDamage = Dimps::Math::FixedToFloat(&value);
            (actor->*Actor::publicMethods.GetVitalityAmt_FixedPoint)(&value);
            sample.health = Dimps::Math::FixedToFloat(&value);
            sample.valid = true;
        }
        meter.Observe(Native::GetNumFramesSimulated_FixedPoint(system)->integral, fighters);
        if (!capture) capture = new TrainingCapture();
        capture->Record(Native::GetNumFramesSimulated_FixedPoint(system)->integral, fighters, meter.View());
    } else if (sampling && delta != 0) {
        meter.Reset();
        ++gapResets;
    }
    sampling = false;
    std::lock_guard<std::mutex> lock(mutex); published = session.GetView(); published.meter = meter.View();
    published.commandId=commandId;published.commandAccepted=commandAccepted;
    if(commandId&&!commandAccepted)published.commandError="Practice state changed. The command was not applied.";
}
void StopCapture() { delete capture; capture = nullptr; }
void CloseBattle() {
    if (session.GetView().available)
        spdlog::info("Training: frame meter reset {} times on multi-frame updates this battle", gapResets);
    gapResets = 0;
    overriding = false; sampling = false;
    if (checkpoint.used) Battle::SaveState::Free(&checkpoint);
    session.Reset(); meter.Reset();
    std::lock_guard<std::mutex> lock(mutex); commands.clear(); published = session.GetView();
}
} }
