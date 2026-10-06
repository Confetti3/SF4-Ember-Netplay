#include "TrainingRuntime.hxx"
#include <algorithm>
#include <cmath>
#include "TrainingCapture.hxx"
#include "ComboCapture.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Game__Battle__Training.hxx"
#include "../Dimps/Dimps__Game__Battle__Trial.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"
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
// Player 1 is the one who practises a combo; player 2 takes it.
TrialSession trial;
ComboCapture comboCapture;
std::uint64_t exportId = 0;
int exportedSlot = -1;
std::vector<Input> exported;
// Reset-on-drop: attempts counted so far and the frames until the restore.
bool resetOnDrop = false;
unsigned attemptsSeen = 0;
int restoreIn = 0;
std::vector<std::string> trialSteps;
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
// The game's own task list for the running trial, driven the way Trial mode
// drives its widget: a fresh movie is asked for, Update creates it once the
// files are in, then the rows are set once and the cursor follows the trial.
// Game memory and game thread only; the overlay's list stands in while it is
// not live.
using TaskList = Dimps::Game::Battle::Trial::TaskList;
using Allocator = Dimps::Platform::Allocator;
struct NativeList {
    TaskList* widget = nullptr;
    std::vector<std::array<std::string, 4>> texts;
    int waited = 0, cursor = -1;
    bool live = false;
} nativeList;
// Frames the movie may take to appear before the overlay's list is kept.
constexpr int NativeListPatience = 300;
void DestroyNativeList() {
    auto& list = nativeList;
    if (list.widget) {
        (list.widget->*TaskList::publicMethods.Release)();
        (list.widget->*TaskList::publicMethods.Destruct)();
        if (auto* allocator = Allocator::staticMethods.GetSingleton()) (allocator->*Allocator::publicMethods.Free)(list.widget);
    }
    list = NativeList{};
}
void StartNativeList(int fighter, std::vector<std::array<std::string, 4>> texts) {
    DestroyNativeList();
    if (fighter < 0 || texts.empty()) return;
    auto* allocator = Allocator::staticMethods.GetSingleton();
    void* memory = allocator ? (allocator->*Allocator::publicMethods.Allocate)(TaskList::Size, 0, -1) : nullptr;
    if (!memory) { spdlog::warn("Training: no game memory for the task list; the overlay's list shows the trial"); return; }
    auto& list = nativeList;
    list.widget = (TaskList*)memory; list.texts = std::move(texts);
    (list.widget->*TaskList::publicMethods.Construct)();
    (list.widget->*TaskList::publicMethods.Init)(fighter);
    *TaskList::GetReloadRequest(list.widget) = 1;
}
// Once per battle frame.
void TickNativeList(int current) {
    auto& list = nativeList;
    if (!list.widget) return;
    (list.widget->*TaskList::publicMethods.Update)();
    if (*TaskList::GetMovieCreated(list.widget)) {
        *TaskList::GetMovieCreated(list.widget) = 0;
        (list.widget->*TaskList::publicMethods.ClearAllTask)();
        // Trial mode sends the first three ids of a row and leaves the fourth empty.
        Dimps::Game::Battle::Trial::Text ids[4];
        for (std::size_t i = 0; i < list.texts.size(); ++i) {
            for (std::size_t slot = 0; slot < 4; ++slot) ids[slot].Set(list.texts[i][slot].c_str(), slot < 3 ? list.texts[i][slot].size() : 0);
            (list.widget->*TaskList::publicMethods.SetTask)(static_cast<int>(i), ids[0], ids[1], ids[2], ids[3]);
        }
        (list.widget->*TaskList::publicMethods.Advance)();
        (list.widget->*TaskList::publicMethods.SetPosition)(150.f, 310.f);
        list.live = true; list.cursor = -1;
    }
    if (!list.live) {
        if (++list.waited == NativeListPatience) {
            spdlog::warn("Training: the game's task list did not appear; the overlay's list shows the trial");
            DestroyNativeList();
        }
        return;
    }
    if (list.cursor != current) { (list.widget->*TaskList::publicMethods.SetTaskCursor)(current); list.cursor = current; }
    (list.widget->*TaskList::publicMethods.Draw)();
}
// A step's ids come from the fighter's command file on the assumption that a
// move's script index is the action id the game reports. Where that fails the
// step just never ticks, so the ids are logged once as the trial ends.
// Back to the checkpoint; false when the engine did not come back whole,
// after which the battle is left rather than played on.
bool RestoreCheckpoint(Native* system) {
    const bool restored = Battle::SaveState::Load(&checkpoint);
    meter.Reset(); trial.Restart();
    if (!restored) {
        spdlog::error("Training: the checkpoint did not fully restore; leaving the battle");
        *Native::GetReadyState(system) = Native::RS_ISLEAVING;
    }
    return restored;
}
void EndTrial() {
    const auto unmatched = trial.Unmatched();
    if (!unmatched.empty()) spdlog::info("Training: trial steps that never matched: {}", unmatched);
    trial = TrialSession{}; trialSteps.clear();
    DestroyNativeList();
}
}
// The game's Training menu settings live in Training::Manager; the dummy
// driver reads them every frame, so a write there is the whole change.
using Manager = Dimps::Game::Battle::Training::Manager;
int* DummyOptions() {
    Manager* manager = Manager::staticMethods.GetSingleton ? Manager::staticMethods.GetSingleton() : nullptr;
    static bool warned = false;
    if (!manager && !warned) { warned = true; spdlog::warn("Training: the game's training options are not reachable; dummy settings stay as they are"); }
    return manager ? Manager::GetOptions(manager) : nullptr;
}
void WriteDummyState(const DummyState& state) {
    int* options = DummyOptions();
    if (!options) return;
    if (state.action >= 0) options[Manager::OPT_ACTION] = state.action;
    if (state.guard >= 0) options[Manager::OPT_GUARD] = state.guard;
    if (state.quickStand >= 0) options[Manager::OPT_QUICK_STAND] = state.quickStand;
    if (state.counterHit >= 0) options[Manager::OPT_COUNTER_HIT] = state.counterHit;
    if (state.stun >= 0) options[Manager::OPT_STUN] = state.stun;
    if (state.super >= 0) options[Manager::OPT_SC_GAUGE] = state.super;
    if (state.revenge >= 0) options[Manager::OPT_REVENGE_GAUGE] = state.revenge;
}
DummyState ReadDummyState(const DummyState& fallback) {
    const int* options = DummyOptions();
    if (!options) return fallback;
    DummyState state;
    state.action = options[Manager::OPT_ACTION]; state.guard = options[Manager::OPT_GUARD];
    state.quickStand = options[Manager::OPT_QUICK_STAND]; state.counterHit = options[Manager::OPT_COUNTER_HIT];
    state.stun = options[Manager::OPT_STUN];
    state.super = options[Manager::OPT_SC_GAUGE]; state.revenge = options[Manager::OPT_REVENGE_GAUGE];
    return state;
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
        if (command.generation == session.GetView().generation &&
            (command.action == Action::StartTrial || command.action == Action::StopTrial)) {
            EndTrial();
            std::string error;
            commandAccepted = command.action == Action::StopTrial ||
                (command.trialSteps.size() == command.trial.steps.size() && trial.Load(command.trial, error));
            if (commandAccepted && command.action == Action::StartTrial) {
                trialSteps = command.trialSteps; resetOnDrop = command.trialResetOnDrop; attemptsSeen = 0; restoreIn = 0;
                if (command.trialTexts.size() == command.trialSteps.size()) StartNativeList(command.trialFighter, command.trialTexts);
            }
            // The overlay ran Load on the same trial and showed its refusal; the
            // command carries no request id, so a refusal here is only logged.
            if (!commandAccepted) spdlog::warn("Training: trial refused: {}", error.empty() ? "the step texts do not match the steps" : error);
            continue;
        }
        if (command.generation == session.GetView().generation && command.action == Action::CaptureStart) { comboCapture.Start(); commandAccepted = true; continue; }
        if (command.generation == session.GetView().generation && command.action == Action::CaptureStop) { comboCapture.Stop(); commandAccepted = true; continue; }
        if (command.generation == session.GetView().generation && command.action == Action::ExportSlot) {
            exportedSlot = session.GetView().selected; exported = session.Slot(exportedSlot); ++exportId; commandAccepted = true; continue;
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
            commandAccepted = RestoreCheckpoint(system);
            if (!commandAccepted) {
            }
        } else if (command.action == Action::ClearHistory) {
            meter.Reset();
        } else if (command.action == Action::DummyState) {
            WriteDummyState(command.dummy);
        }
    }
    TickNativeList(trial.GetView().current);
    // Opening either overlay suspends recording; native pause frames are
    // also excluded by the before/after simulation counter check.
    if (!session.GetView().ready) return;
    beforeFrame = Native::GetNumFramesSimulated_FixedPoint(system)->integral;
    sampling = true;
    // Playback needs no pad, so it goes on under the open controls, where a
    // replay or trial can be watched; recording waits for the pad to be free.
    const bool menu = sf4e::Overlay::CapturesMenuInput() || sf4e::Pad::MenuInputBlocked();
    if (menu && session.GetView().mode != Mode::Playback) return;
    Pad* pad = Pad::staticMethods.GetSingleton();
    Frame physical;
    for (int side = 0; side < 2 && !menu; ++side) {
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
                if (script && script[2] > 0 && script[2] <= script[3] && script[3] <= 4096) sample.interruptibleFrame = script[2];
            }
            (actor->*Actor::publicMethods.GetDamage)(&value);
            sample.damage = Dimps::Math::FixedToFloat(&value);
            (actor->*Actor::publicMethods.GetComboDamage)(&value);
            sample.comboDamage = Dimps::Math::FixedToFloat(&value);
            (actor->*Actor::publicMethods.GetVitalityAmt_FixedPoint)(&value);
            sample.health = Dimps::Math::FixedToFloat(&value);
            sample.valid = true;
        }
        // The replay's waiting frames read the fight: the fighter playing it
        // in a neutral state, and a hit on the other one landing this frame.
        {
            static std::array<FighterSample, 2> previous;
            const int side = session.GetView().playbackSide;
            const auto& own = fighters[side], & other = fighters[1 - side];
            const bool hit = other.valid && ((ClassifyStatus(other.status) == Phase::Hit && ClassifyStatus(previous[1 - side].status) != Phase::Hit) ||
                other.comboDamage > previous[1 - side].comboDamage);
            // How soon the script says the fighter is free again, so a link's press can land on that frame.
            const int until = own.valid && own.interruptibleFrame > own.actionFrame ? static_cast<int>(std::ceil(own.interruptibleFrame - own.actionFrame)) : -1;
            session.Observe(own.valid && ClassifyStatus(own.status) == Phase::Neutral, hit, until);
            previous = fighters;
        }
        if (commitInput) session.Commit(output);
        meter.Observe(Native::GetNumFramesSimulated_FixedPoint(system)->integral, fighters);
        trial.Observe(ObserveTrial(fighters[0], fighters[1]));
        comboCapture.Observe(fighters[0]);
        // An attempt just ended: the restore waits long enough for the result
        // to be read, then the next attempt starts from the checkpoint.
        const unsigned attemptsDone = trial.GetView().failures + trial.GetView().successes;
        if (resetOnDrop && checkpoint.used && attemptsDone != attemptsSeen) { attemptsSeen = attemptsDone; restoreIn = 45; }
        if (restoreIn && !--restoreIn) RestoreCheckpoint(system);
        if (!capture) capture = new TrainingCapture();
        capture->Record(Native::GetNumFramesSimulated_FixedPoint(system)->integral, fighters, meter.View());
    } else if (sampling && delta != 0) {
        meter.Reset(); trial.Restart();
        ++gapResets;
    }
    sampling = false;
    std::lock_guard<std::mutex> lock(mutex); published = session.GetView(); published.meter = meter.View();
    if (published.available) published.dummy = ReadDummyState(published.dummy);
    published.capturing = comboCapture.Active(); published.captured.clear();
    for (const auto& event : comboCapture.Events()) published.captured.emplace_back(event.action, event.cancel);
    published.exportId = exportId; published.exportedSlot = exportedSlot; published.exported = exported;
    published.trialSteps = trialSteps; published.trial = trial.GetView(); published.nativeTrialList = nativeList.live;
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
    session.Reset(); meter.Reset(); EndTrial();
    std::lock_guard<std::mutex> lock(mutex); commands.clear(); published = session.GetView();
}
} }
