#include "TrainingRuntime.hxx"
#include <algorithm>
#include <cmath>
#include "TrainingCapture.hxx"
#include "BacFile.hxx"
#include "GameFiles.hxx"
#include "ConfirmedSamples.hxx"
#include <atomic>
#include "../common/FighterCatalog.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Game__Battle__Training.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../Dimps/Dimps__Sound.hxx"
#include "../sf4e/sf4e__Game__Battle__System.hxx"
#include "../sf4e/sf4e__Overlay.hxx"
#include "../sf4e/sf4e__Pad.hxx"
#include <mutex>
#include <random>
#include <spdlog/spdlog.h>

namespace sf4e { namespace training {
namespace {
using Native = Dimps::Game::Battle::System;
using Battle = sf4e::Game::Battle::System;
using Pad = Dimps::Pad::System;
// The meter's own copy of the native status values must stay the game's.
using NativeActor = Dimps::Game::Battle::Chara::Actor;
static_assert(ActorStatus::Stand == NativeActor::AS_STAND && ActorStatus::Crouch == NativeActor::AS_CROUCH &&
    ActorStatus::Jump == NativeActor::AS_JUMP && ActorStatus::StandToCrouch == NativeActor::AS_STAND_TO_CROUCH &&
    ActorStatus::CrouchToStand == NativeActor::AS_CROUCH_TO_STAND && ActorStatus::StandToJump == NativeActor::AS_STAND_TO_JUMP &&
    ActorStatus::JumpToStand == NativeActor::AS_JUMP_TO_STAND && ActorStatus::TurnStand == NativeActor::AS_TURN__STAND &&
    ActorStatus::TurnCrouch == NativeActor::AS_TURN__CROUCH && ActorStatus::TurnWalk == NativeActor::AS_TURN__WALK &&
    ActorStatus::Forward == NativeActor::AS_FORWARD && ActorStatus::Backward == NativeActor::AS_BACKWARD &&
    ActorStatus::FrontDash == NativeActor::AS_FRONTDASH && ActorStatus::BackDash == NativeActor::AS_BACKDASH &&
    ActorStatus::GuardStand == NativeActor::AS_GUARD__STAND && ActorStatus::GuardCrouch == NativeActor::AS_GUARD__CROUCH &&
    ActorStatus::Skill == NativeActor::AS_SKILL && ActorStatus::Stun == NativeActor::AS_STUN &&
    ActorStatus::Bound == NativeActor::AS_BOUND && ActorStatus::Down == NativeActor::AS_DOWN &&
    ActorStatus::Rise == NativeActor::AS_RISE && ActorStatus::Damage == NativeActor::AS_DAMAGE &&
    ActorStatus::DamageGuard == NativeActor::AS_DAMAGE__GUARD && ActorStatus::DamageBlow == NativeActor::AS_DAMAGE__BLOW &&
    ActorStatus::Sequence == NativeActor::AS_SEQUENCE, "FrameMeter.hxx ActorStatus differs from Actor::Status");
Session session;
FrameMeter meter;
std::uint64_t exportId = 0;
int exportedSlot = -1;
std::vector<Input> exported;
// Leave: frames until the battle is sent to the main menu, 0 when it is not.
// The announcer's call and the banner play first, as the game's own fight
// request lets its banner play before it takes the battle away.
constexpr int LeaveFrames = 120;
int leaveIn = 0;
// Each fighter's script file, for the moves whose native header names no
// attack frames: a fireball's hitbox is in the effect the move spawns.
// scriptFighter: whose file is held, -1 none. Kept across battles.
bac::File scriptFiles[2];
int scriptFighter[2] = {-1, -1};
// The meter in a rollback match: asked for by the player, and fed only
// frames whose inputs are confirmed. watching is read on the game thread
// and set from the overlay's.
std::atomic<bool> watching{false};
bool matchShown = false;
ConfirmedSamples confirmed;
// A match under a table's Training rule. It has no shared checkpoint: the
// offline checkpoint is not part of the rollback state.
std::atomic<bool> matchPractice{false};
// The dummy's own behaviour. The plan is the player's and outlives a battle;
// offline practice only, so the random numbers need not be reproducible.
DummyPlan dummyPlan;
DummyWatch dummyWatch;
std::minstd_rand dummyRandom{std::random_device{}()};
// The stretch of stun a reply was last decided for, so each gets one roll.
unsigned decidedStretch = 0;
DummyAction dummyAction;
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
Acks acks;
// Updates that advanced more than one frame, each of which resets the frame
// meter. Logged per battle: a large count explains a missing frame-advantage
// readout (ledger F-006).
std::uint64_t gapResets = 0;
// Puts both fighters at an x each, through the root position the engine
// keeps for them. Only x, on the ground; add y and facing if they are ever
// needed.
bool PlaceFighters(Native* system, const float* x) {
    using Actor = Dimps::Game::Battle::Chara::Actor;
    using Unit = Dimps::Game::Battle::Chara::Unit;
    Unit* unit = (system->*Native::publicMethods.GetCharaUnit)();
    if (!unit) return false;
    for (unsigned side = 0; side < 2; ++side) {
        Actor* actor = (unit->*Unit::publicMethods.GetActorByIndex)(side);
        float* position = actor ? (actor->*Actor::publicMethods.GetCurrentRootPosition)() : nullptr;
        if (!position) return false;
        position[0] = x[side];
    }
    return true;
}
// Back to the checkpoint; false when the engine did not come back whole,
// after which the battle is left rather than played on.
bool RestoreCheckpoint(Native* system) {
    const bool restored = Battle::SaveState::Load(&checkpoint);
    meter.Reset(); dummyWatch.Reset();
    if (!restored) {
        spdlog::error("Training: the checkpoint did not fully restore; leaving the battle");
        *Native::GetReadyState(system) = Native::RS_ISLEAVING;
    }
    return restored;
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
    if (state.action >= 0) dummyAction.Set(options[Manager::OPT_ACTION], state.action);
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
    state.action = dummyAction.Read(options[Manager::OPT_ACTION]); state.guard = options[Manager::OPT_GUARD];
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
// Applies one command from the panel or the hotkeys; whether it took effect.
static bool Dispatch(Native* system, const Command& command) {
    if (command.generation == session.GetView().generation && command.action == Action::AutoFreeze) {
        meter.SetAutoFreeze(command.value != 0); return true;
    }
    if (command.generation == session.GetView().generation && command.action == Action::Place) return PlaceFighters(system, command.place);
    if (command.generation == session.GetView().generation && command.action == Action::DummyPlan) {
        if (!ValidDummyPlan(command.plan)) return false;
        dummyPlan = command.plan; return true;
    }
    if (command.generation == session.GetView().generation && command.action == Action::Leave) {
        // Once per battle; value is the announcer's volume in percent, 0 for none.
        if (leaveIn) return false;
        leaveIn = LeaveFrames;
        const bool called = command.value > 0 && Dimps::Sound::PlaySystemCue(Dimps::Sound::SystemCue::HereComesChallenger,
            Dimps::Sound::SystemChannel::Voice, command.value / 100.f);
        spdlog::info("Training: leaving for the main menu in {} frames; challenger call {}", LeaveFrames, called ? "played" : "not played");
        return true;
    }
    if (command.generation == session.GetView().generation && command.action == Action::ExportSlot) {
        exportedSlot = session.GetView().selected; exported = session.Slot(exportedSlot); ++exportId; return true;
    }
    if (!session.Apply(command)) {
        // A position that was asked for and not saved or put back is the
        // one refusal a player notices without being told.
        if (command.action == Action::Save || command.action == Action::Restore)
            spdlog::info("Training: {} position refused (fight ready={}, saved={}, command for battle {} in battle {})",
                command.action == Action::Save ? "save" : "reset", session.GetView().ready, session.GetView().checkpoint,
                command.generation, session.GetView().generation);
        return false;
    }
    if (command.action == Action::Save) {
        if (checkpoint.used) Battle::SaveState::Free(&checkpoint);
        // A state the memento cannot represent is refused, not kept
        // without its task functors (ledger A-001).
        const bool saved = Battle::SaveState::Save(&checkpoint);
        // The checkpoint is loaded long after: a voice that had ended by then must not come back with it.
        if (saved) Battle::SaveState::ForgetFinishedSounds(&checkpoint);
        session.SetCheckpoint(checkpoint.used);
        spdlog::info("Training: position {}", saved ? "saved" : "not saved, the game's state could not be taken");
        return saved;
    } else if (command.action == Action::Restore) {
        const bool restored = RestoreCheckpoint(system);
        spdlog::info("Training: position {}", restored ? "reset to the saved one" : "not reset");
        return restored;
    } else if (command.action == Action::ClearHistory) {
        meter.Reset();
    } else if (command.action == Action::DummyState) {
        WriteDummyState(command.dummy);
    }
    return true;
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
    for (const auto& command : pending) acks.Note(command.requestId, Dispatch(system, command));
    if (!session.Replying()) if (int* options = DummyOptions()) dummyAction.EndReply(options[Manager::OPT_ACTION]);
    // The pause menu's "exit to main menu", asked for by Ember instead of the
    // player. Only out of a fight that is running: a battle still loading or
    // in its intro is waited for, as that menu cannot be opened there either.
    if (leaveIn && (leaveIn > 1 || session.GetView().ready) && !--leaveIn) {
        *Native::GetBattleExitType(system) = Native::BET_PAUSE_TOMAINMENU;
        *Native::GetReadyState(system) = Native::RS_ISLEAVING;
        spdlog::info("Training: battle told to leave for the main menu");
    }
    // Opening either overlay suspends recording; native pause frames are
    // also excluded by the before/after simulation counter check.
    if (!session.GetView().ready) return;
    beforeFrame = Native::GetNumFramesSimulated_FixedPoint(system)->integral;
    sampling = true;
    // Playback needs no pad, so it goes on under the open controls, where it
    // can be watched; recording waits for the pad to be free.
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
// The fighter a player brought into this battle; -1 when the request holds
// no fighter the catalog knows.
int BattleFighter(Native* system, int side) {
    auto** request = Native::GetRequest(system);
    if (!request || !*request) return -1;
    const int id = Dimps::Game::Request::GetFighterId(*request, side);
    return selection::FindFighter(id) ? id : -1;
}
// Two files under a megabyte, read in the battle's first sampled frame; move
// the read to a worker if that frame is ever seen to stutter.
const bac::File& FighterScripts(Native* system, unsigned side) {
    const int id = BattleFighter(system, static_cast<int>(side));
    if (id == scriptFighter[side]) return scriptFiles[side];
    scriptFighter[side] = id; scriptFiles[side] = {};
    const auto* fighter = selection::FindFighter(id);
    if (!fighter) return scriptFiles[side];
    wchar_t program[32768] = {};
    GetModuleFileNameW(nullptr, program, 32768);
    const std::string code = fighter->code;
    const auto folder = CommandFolder(std::filesystem::path(program).parent_path(), code);
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!ReadFile(folder / (code + ".bac"), bac::MaxBytes, bytes, error) || !bac::Read(bytes.data(), bytes.size(), scriptFiles[side], error))
        spdlog::warn("Training: no projectile frames for {}: {}", code, error);
    return scriptFiles[side];
}
// Both fighters as the game holds them after an update. Only reads.
// files: whether a move without attack frames may take them from the
// fighter's script file.
std::array<FighterSample, 2> ReadFighters(Native* system, bool files) {
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
            } else if (files && script && script[3] <= 4096 &&
                bac::ProjectileBoundary(FighterScripts(system, side), sample.action, sample.firstActiveFrame, sample.lastActiveFrame)) {
                if (sample.firstActiveFrame < script[3]) sample.boundaryProvenance = BoundaryProvenance::BacEffectSpawn;
                else sample.firstActiveFrame = sample.lastActiveFrame = -1;
            }
            if (script && script[2] > 0 && script[2] <= script[3] && script[3] <= 4096) sample.interruptibleFrame = script[2];
            if (script && script[3] > 0 && script[3] <= 4096) sample.totalFrames = script[3];
        }
        (actor->*Actor::publicMethods.GetDamage)(&value);
        sample.damage = Dimps::Math::FixedToFloat(&value);
        (actor->*Actor::publicMethods.GetComboDamage)(&value);
        sample.comboDamage = Dimps::Math::FixedToFloat(&value);
        (actor->*Actor::publicMethods.GetVitalityAmt_FixedPoint)(&value);
        sample.health = Dimps::Math::FixedToFloat(&value);
        sample.valid = true;
    }
    return fighters;
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
        std::array<FighterSample, 2> fighters = ReadFighters(system, true);
        {
            float x[2] = {0, 0};
            for (unsigned side = 0; side < 2 && unit; ++side)
                if (Actor* actor = (unit->*Unit::publicMethods.GetActorByIndex)(side)) if (const float* position = (actor->*Actor::publicMethods.GetCurrentRootPosition)()) x[side] = position[0];
            session.SetPositions(x[0], x[1]);
        }
        // A playback's waiting frames read the fight: the fighter playing it
        // in a neutral state, and a hit on the other one landing this frame.
        {
            static std::array<FighterSample, 2> previous;
            const int side = session.GetView().playbackSide;
            const auto& own = fighters[side], & other = fighters[1 - side];
            const bool hit = other.valid && ((ClassifyStatus(other.status) == Phase::Hit && ClassifyStatus(previous[1 - side].status) != Phase::Hit) ||
                other.comboDamage > previous[1 - side].comboDamage);
            // How soon the script says the fighter is free again, so a link's
            // press can land on that frame: its interruptible frame, else its end.
            const int boundary = own.interruptibleFrame > 0 ? own.interruptibleFrame : own.totalFrames;
            const int until = own.valid && boundary > own.actionFrame ? static_cast<int>(std::ceil(boundary - own.actionFrame)) : -1;
            session.Observe(own.valid && ClassifyStatus(own.status) == Phase::Neutral, hit, until);
            previous = fighters;
        }
        if (commitInput) session.Commit(output);
        meter.Observe(Native::GetNumFramesSimulated_FixedPoint(system)->integral, fighters);
        // The dummy is free again: it may reply, and it may change its stance.
        // Its stun is timed, so the reply's motion can go in before the free
        // frame and its button land on it; a stun not seen before is replied
        // to as it ends.
        {
            const DummySeen seen = dummyWatch.Observe(fighters[1], fighters[0].action);
            int* options = DummyOptions();
            // Hit again: a reply already begun is dropped, and this stretch decides anew.
            if (seen.held && seen.stretch != decidedStretch) session.StopReply();
            if (!session.Replying() && options) dummyAction.EndReply(options[Manager::OPT_ACTION]);
            const bool typed = !dummyPlan.moves[0].empty();
            const auto& frames = typed ? dummyPlan.moves[session.GetView().x[1] < session.GetView().x[0] ? 0 : 1] : session.Slot(dummyPlan.slot);
            const int cause = seen.freed ? seen.freed : seen.held;
            if (cause && seen.stretch != decidedStretch && ReplyDue(seen, ReplyLead(frames), dummyPlan.timing)) {
                decidedStretch = seen.stretch;
                if (DummyReplies(dummyPlan, cause, dummyRandom()) &&
                    (typed ? session.Reply(frames) : session.Reply(dummyPlan.slot)) && options) {
                    dummyAction.BeginReply(options[Manager::OPT_ACTION]);
                }
            }
            if (seen.freed && dummyPlan.varyStance && options) {
                const int action = dummyAction.Read(options[Manager::OPT_ACTION]);
                if (action == 0 || action == 1) dummyAction.Set(options[Manager::OPT_ACTION], dummyRandom() & 1);
            }
        }
        if (!capture) capture = new TrainingCapture();
        capture->Record(Native::GetNumFramesSimulated_FixedPoint(system)->integral, fighters, meter.View());
    } else if (sampling && delta != 0) {
        meter.Reset(); dummyWatch.Reset();
        ++gapResets;
    }
    sampling = false;
    std::lock_guard<std::mutex> lock(mutex); published = session.GetView(); published.meter = meter.View();
    if (published.available) published.dummy = ReadDummyState(published.dummy);
    published.exportId = exportId; published.exportedSlot = exportedSlot; published.exported = exported;
    published.leavingIn = leaveIn;
    published.acks=acks;
}
void SetMatchPractice(bool enabled) { matchPractice = enabled; }
bool MatchPracticeActive() { return matchPractice; }
unsigned ClearReservedInputBits(unsigned raw) {
    return matchPractice ? raw & ~ReservedInputBits : raw;
}
void WatchMatches(bool enabled) { watching = enabled; }
void ObserveMatch(Native* system, int stateFrame, int lastConfirmedInput) {
    if (!watching) {
        // Turned off mid-match: the meter goes, and starts clean if it is asked for again.
        if (!matchShown) return;
        matchShown = false; meter.Reset(); confirmed.Reset();
        std::lock_guard<std::mutex> lock(mutex); published.watching = false;
        return;
    }
    // No script-file fallback here, so a projectile move without attack
    // frames shows no startup in a match; the file read does not belong in a
    // rollback frame. Load it at battle start if it is missed.
    const auto fighters = ReadFighters(system, false);
    // A spectator plays confirmed inputs only and has no save frame to name one by.
    if (stateFrame <= 0) meter.Observe(Native::GetNumFramesSimulated_FixedPoint(system)->integral, fighters);
    else {
        confirmed.Capture(stateFrame, fighters);
        int frame = 0; std::array<FighterSample, 2> next;
        if (!confirmed.Next(lastConfirmedInput, frame, next)) return;
        do meter.Observe(frame, next); while (confirmed.Next(lastConfirmedInput, frame, next));
    }
    std::lock_guard<std::mutex> lock(mutex);
    published.meter = meter.View(); published.watching = matchShown = true;
}
void StopCapture() { delete capture; capture = nullptr; }
void CloseBattle() {
    if (session.GetView().available)
        spdlog::info("Training: frame meter reset {} times on multi-frame updates this battle", gapResets);
    gapResets = 0; leaveIn = 0;
    overriding = false; sampling = false;
    if (checkpoint.used) Battle::SaveState::Free(&checkpoint);
    if (session.GetView().available) if (int* options = DummyOptions()) dummyAction.EndReply(options[Manager::OPT_ACTION]);
    session.Reset(); meter.Reset(); confirmed.Reset(); matchShown = false;
    matchPractice = false; dummyWatch.Reset(); dummyAction = DummyAction{};
    std::lock_guard<std::mutex> lock(mutex); commands.clear(); published = session.GetView();
}
} }
