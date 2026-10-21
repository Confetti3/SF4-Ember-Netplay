#include "TrainingRuntime.hxx"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "TrainingCapture.hxx"
#include "FighterPlacement.hxx"
#include "BacFile.hxx"
#include "GameFiles.hxx"
#include "ConfirmedSamples.hxx"
#include "DummyReply.hxx"
#include "TrainingCommands.hxx"
#include <atomic>
#include "../common/FighterCatalog.hxx"
#include "../Dimps/Dimps__Game__Battle__Camera.hxx"
#include "../Dimps/Dimps__Game__Battle__Effect.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Game__Battle__Training.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../Dimps/Dimps__Sound.hxx"
#include "../sf4e/sf4e__Game__Battle__System.hxx"
#include "../sf4e/sf4e__InputDevices.hxx"
#include "../sf4e/sf4e__InputLag.hxx"
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
// The dummy fighters' slots, kept between battles (RecordingBanks).
RecordingBanks banks;
FrameMeter meter;
std::uint64_t exportId = 0;
int exportedSlot = -1;
std::vector<Input> exported;
// Frames until the battle is sent to the main menu, 0 when it is not.
// The announcer's call and the banner play first, as the game's own fight
// request lets its banner play before it takes the battle away; go now cuts
// the banner short.
constexpr int LeaveFrames = 120;
LeaveCountdown leaving;
// The call that stands for this battle, as the room's call owner last set it (SetCall).
CallControl callControl;
// Each fighter's script file, for the moves whose native header names no
// attack frames: a fireball's hitbox is in the effect the move spawns.
// scriptFighter: whose file is held, -1 none. Kept across battles.
bac::File scriptFiles[2];
int scriptFighter[2] = {-1, -1};
// The meter in a rollback match: asked for by the player, and fed only
// frames whose inputs are confirmed. watching is read on the game thread
// and set from the overlay's.
std::atomic<bool> watching{false};
// The frames the meter keeps, as the Frame data page chose (SetMeterHistory).
std::atomic<int> meterHistory{static_cast<int>(MeterHistory)};
std::atomic<bool> meterContinuous{false};
// Whether Hitboxes is on (ShowHitBoxes): the boxes are read only then, since
// reading them walks the game's box lists every frame.
std::atomic<bool> hitBoxesShown{false};
// Training and the match meter both feed the one meter, so each applies the player's choice before it observes.
void ApplyMeterSettings() {
    meter.SetHistory(static_cast<std::size_t>(meterHistory.load())); meter.SetContinuous(meterContinuous);
}
#ifdef SF4E_SELFTEST
// The in-game self-test's: the lab also runs in an offline Versus battle.
bool versusForTest = false;
// The self-test's: the boxes are read whatever Hitboxes says.
bool boxesForTest = false;
#endif
// Where the lab runs: a Training battle (and the self-test's Versus battle,
// in a self-test build), not while the battle is being left. The one rule for
// the lab and its controls.
bool LabBattle(Native* system) {
    const int gameMode = (system->*Native::publicMethods.GetGameMode)();
#ifdef SF4E_SELFTEST
    const bool lab = gameMode == Dimps::Game::Battle::GAMEMODE_TRAINING || (versusForTest && gameMode == Dimps::Game::Battle::GAMEMODE_VERSUS);
#else
    const bool lab = gameMode == Dimps::Game::Battle::GAMEMODE_TRAINING;
#endif
    return lab && !(system->*Native::publicMethods.IsLeavingBattle)();
}
// Whether the fighters' boxes are read this frame: while Hitboxes is on, and
// always in a self-test that checks them.
bool BoxesWanted() {
#ifdef SF4E_SELFTEST
    if (boxesForTest) return true;
#endif
    return hitBoxesShown;
}
bool matchShown = false;
ConfirmedSamples confirmed;
// A Practice room's match. It has no shared checkpoint: the
// offline checkpoint is not part of the rollback state.
std::atomic<bool> matchPractice{false};
// The dummy's own behaviour. The plan is the player's and outlives a battle;
// offline practice only, so the random numbers need not be reproducible.
DummyPlan dummyPlan;
DummyWatch dummyWatch;
ReplyDecision replies;
ReplyTrace replyTrace;
std::minstd_rand dummyRandom{std::random_device{}()};
DummyControl dummyControl;
// Both fighters as the last update left them, which a playback's waits read
// a hit against (AfterUpdate); a reset reseeds them from the restored state.
std::array<FighterSample, 2> playbackSeen;
// Normal shutdown owns this worker. Never join a thread from a DLL destructor
// while Windows holds the loader lock.
TrainingCapture* capture = nullptr;
std::mutex mutex;
View published;
CommandQueue commands;
// Separate ownership from the GGPO pool and the developer save slot.
Battle::SaveState checkpoint;
Frame output;
thread_local bool overriding = false;
bool sampling = false;
bool commitInput = false;
// The battle's frame before this update, once the update's commands ran; set
// when the update is Training's (following).
int beforeFrame = 0;
bool following = false;
Acks acks;
// Updates that advanced more than one frame, each of which resets the frame
// meter. Logged per battle: a large count explains a missing frame-advantage
// readout (ledger F-006).
std::uint64_t gapResets = 0;
// Updates since the meter last took a frame that simulated frames all the
// same: not ready (no fight running), or more than one frame each. A field
// report of a meter that stopped moving then shows whether it was fed and
// holding, or not fed at all; logged once a stretch, at MeterUnfedLogUpdates.
constexpr std::uint64_t MeterUnfedLogUpdates = 600;
std::uint64_t unfedUpdates = 0, unfedNotReady = 0, unfedMultiFrame = 0;
// The root position the engine keeps for each fighter, which a Place moves
// (MoveFighters: only x, on the ground; add y and facing if they are ever
// needed) and the session's facing is read from.
FighterRoots ReadRoots(Native* system) {
    using Actor = Dimps::Game::Battle::Chara::Actor;
    using Unit = Dimps::Game::Battle::Chara::Unit;
    FighterRoots roots{};
    Unit* unit = (system->*Native::publicMethods.GetCharaUnit)();
    for (unsigned side = 0; side < 2 && unit; ++side)
        if (Actor* actor = (unit->*Unit::publicMethods.GetActorByIndex)(side)) roots[side] = (actor->*Actor::publicMethods.GetCurrentRootPosition)();
    return roots;
}
// Where the fighters stand (x) for the session, which turns what it plays by them.
void FollowPositions(Native* system) {
    const FighterRoots roots = ReadRoots(system);
    session.SetPositions(roots[0] ? roots[0][0] : 0, roots[1] ? roots[1][0] : 0);
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
// Player 2's control back to the one the Training menu set, as the game puts
// it when the pause menu closes.
void ReturnDummyControl(Native* system) {
    int* control = Native::GetPlayerControl(system, 1);
    if (!control) return;
    const int was = *control;
    *control = *Native::GetMenuPlayerControl(system, 1);
    spdlog::info("Training: Player 2's control back to {} from {}", *control, was);
}
// Back to the checkpoint; false when the engine did not come back whole,
// after which the battle is left rather than played on. The checkpoint puts
// back the dummy action it was saved with, and the dummy's control is not in
// it: the action found before it stands, so the two still agree. The input
// the game holds for the next frame is the checkpoint's, so Player 2 takes
// its own control back and plays from the pad again only from what Ember
// plays after the reset (FollowDummy).
bool RestoreCheckpoint(Native* system) {
    int* options = DummyOptions();
    const int action = options ? options[Manager::OPT_ACTION] : -1;
    const bool restored = Battle::SaveState::Load(&checkpoint);
    if (options) options[Manager::OPT_ACTION] = action;
    if (dummyControl.Rewind()) ReturnDummyControl(system);
    meter.Reset(); ForgetStun(dummyWatch, session, replies); replyTrace.Reset();
    if (!restored) {
        spdlog::error("Training: the checkpoint did not fully restore; leaving the battle");
        *Native::GetReadyState(system) = Native::RS_ISLEAVING;
    }
    return restored;
}
// What Ember plays on the fighters, as the log names it.
struct Playing {
    Mode mode = Mode::Idle; int slot = -1, side = 1; bool reply = false;
    bool operator==(const Playing& other) const { return mode == other.mode && slot == other.slot && side == other.side && reply == other.reply; }
};
Playing playing;
void LogPlaying(const Playing& what, const char* change) {
    if (what.mode == Mode::Idle) return;
    const char* kind = what.mode == Mode::Standby ? "standby" : what.mode == Mode::Recording ? "recording" : what.reply ? "reply" : "playback";
    if (what.slot == ReplySlot) spdlog::info("Training: {} of typed moves {}", kind, change);
    else spdlog::info("Training: {} of slot {} on Player {} {}", kind, what.slot + 1, what.side + 1, change);
}
// At the start of each update, before its commands can reset the battle: a
// different action picked in the pause menu while Ember plays Player 2
// stands, and what Ember plays stops.
void ReconcileDummy(Native* system) {
    int* options = DummyOptions();
    if (!options || !dummyControl.Reconcile(options[Manager::OPT_ACTION])) return;
    spdlog::info("Training: dummy action set to {} in the pause menu; it stands and Ember stops playing Player 2", options[Manager::OPT_ACTION]);
    ReturnDummyControl(system);
    session.Apply({Action::Stop, session.GetView().generation});
}
// Each update, once its input is planned: Player 2 plays from the pad while
// the game reads what Ember plays there and the fighters act on it
// (DummyControl), from the update that reads its first input to the one that
// acts on its last, and the dummy's own control is put back after.
// supplies: InputPlan::suppliesDummy.
void FollowDummy(Native* system, bool supplies) {
    const auto& view = session.GetView();
    const Playing now{view.mode, PadPlaysDummy(view.mode) ? view.selected : view.active, view.playbackSide, session.Replying()};
    if (!(now == playing)) { LogPlaying(playing, "ended"); LogPlaying(now, "started"); playing = now; }
    int* options = DummyOptions();
    int* control = Native::GetPlayerControl(system, 1);
    if (!options || !control) return;
    const int action = options[Manager::OPT_ACTION];
    switch (dummyControl.Follow(supplies, action)) {
    case DummyControl::Step::Begin:
        spdlog::info("Training: Player 2 plays from the pad (control {} to {}, action {})",
            *control, static_cast<int>(Native::PC_PAD), action);
        *control = Native::PC_PAD;
        break;
    // The pause menu closing puts the menu's control back over it.
    case DummyControl::Step::Keep:
        if (*control != Native::PC_PAD) spdlog::info("Training: Player 2's control {} set again to {}", *control, static_cast<int>(Native::PC_PAD));
        *control = Native::PC_PAD;
        break;
    case DummyControl::Step::End: ReturnDummyControl(system); break;
    case DummyControl::Step::None: break;
    }
}
}
void WriteDummyState(const DummyState& state) {
    int* options = DummyOptions();
    if (!options) return;
    if (state.action >= 0) dummyControl.Set(options[Manager::OPT_ACTION], state.action);
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
void SetCall(const CallControl& call) { std::lock_guard<std::mutex> lock(mutex); callControl = call; }
Place ReadPlace() {
    std::lock_guard<std::mutex> lock(mutex);
    Place place; place.generation = published.generation; place.x[0] = published.x[0]; place.x[1] = published.x[1];
    return place;
}
bool ChordStartsRecording() { std::lock_guard<std::mutex> lock(mutex); return ChordStartsRecording(published.mode); }
bool ControlsAvailable() {
    if(!session.GetView().available || Battle::ggpo) return false;
    auto* system=Native::staticMethods.GetSingleton();
    return system && LabBattle(system);
}
bool Submit(Command command) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!published.available || command.generation != published.generation) return false;
    return commands.Push(command);
}
bool ReadOverride(int side, Input& result) {
    if (!overriding || side < 0 || side >= 2) return false;
    result = output[side]; return true;
}
// Applies one command from the panel or the hotkeys; whether it took effect.
// The session checks every command once, for its battle and whether it can
// apply now; only then is the game touched.
std::array<FighterSample, 2> ReadFighters(Native* system, bool files);
int BattleFighter(Native* system, int side);
static bool Dispatch(Native* system, const Command& command) {
    if (!session.Apply(command)) {
        // A position that was asked for and not saved or put back is the
        // one refusal a player notices without being told.
        if (command.action == Action::Save || command.action == Action::Restore)
            spdlog::info("Training: {} position refused (fight ready={}, saved={}, command for battle {} in battle {})",
                command.action == Action::Save ? "save" : "reset", session.GetView().ready, session.GetView().checkpoint,
                command.generation, session.GetView().generation);
        return false;
    }
    switch (command.action) {
    case Action::Place: return MoveFighters(session, ReadRoots(system), command.place);
    case Action::DummyPlan: dummyPlan = command.plan; return true;
    case Action::ExportSlot:
        exportedSlot = session.GetView().selected; exported = session.Slot(exportedSlot); ++exportId; return true;
    case Action::Save: {
        if (checkpoint.used) Battle::SaveState::Free(&checkpoint);
        // A state the memento cannot represent is refused, not kept
        // without its task functors (ledger A-001).
        const bool saved = Battle::SaveState::Save(&checkpoint);
        // The checkpoint is loaded long after: a voice that had ended by then must not come back with it.
        if (saved) Battle::SaveState::ForgetFinishedSounds(&checkpoint);
        session.SetCheckpoint(checkpoint.used);
        spdlog::info("Training: position {}", saved ? "saved" : "not saved, the game's state could not be taken");
        return saved;
    }
    case Action::Restore: {
        const bool restored = RestoreCheckpoint(system);
        playbackSeen = ReadFighters(system, false);
        // The playback starting again plays for the sides the reset put back.
        FollowPositions(system);
        spdlog::info("Training: position {}", restored ? "reset to the saved one" : "not reset");
        return restored;
    }
    case Action::ClearHistory: meter.Reset(); return true;
    case Action::DummyState: WriteDummyState(command.dummy); return true;
    default: return true;
    }
}
void BeforeUpdate(Native* system, bool networkOwned) {
    overriding = false; sampling = false; commitInput = false; following = false;
    const bool available = !networkOwned && LabBattle(system);
    if (!available) {
        if (session.GetView().available) CloseBattle();
        return;
    }
    // The dummy fighter's slots from its last battle (CloseBattle keeps them).
    if (!session.GetView().available) {
        const int fighter = BattleFighter(system, 1);
        session.Enter(banks.Attach(fighter)); session.Seed(dummyRandom());
        if (fighter < 0) spdlog::info("Training: the dummy's fighter is not known; its slots last this battle only");
    }
    session.SetReady((system->*Native::publicMethods.IsFight)() &&
        *Native::GetReadyState(system) == Native::RS_FIGHT);
    ReconcileDummy(system);
    std::deque<Command> taken; std::vector<std::uint64_t> replaced;
    { std::lock_guard<std::mutex> lock(mutex); commands.Take(taken, replaced); }
    RunBatch(PrepareBatch(taken, replaced, session.GetView().generation), acks, [system](const Command& command) { return Dispatch(system, command); });
    // The countdown follows the call that stands for this battle, before it
    // ticks, so a call that has ended can never take the battle away and a
    // go now taken for the call is never lost (TrainingSession.hxx).
    CallControl call;
    { std::lock_guard<std::mutex> lock(mutex); call = callControl; }
    if (leaving.Follow(call, session.GetView().generation, LeaveFrames)) {
        const bool called = call.volume > 0 && Dimps::Sound::PlaySystemCue(Dimps::Sound::SystemCue::HereComesChallenger,
            Dimps::Sound::SystemChannel::Voice, call.volume / 100.f);
        spdlog::info("Training: leaving for the main menu in {} frames for call {}; challenger call {}", LeaveFrames, call.call, called ? "played" : "not played");
    }
    // The pause menu's "exit to main menu", asked for by Ember instead of the
    // player. Only out of a fight that is running: a battle still loading or
    // in its intro is waited for, as that menu cannot be opened there either.
    if (leaving.Tick(session.GetView().ready)) {
        *Native::GetBattleExitType(system) = Native::BET_PAUSE_TOMAINMENU;
        *Native::GetReadyState(system) = Native::RS_ISLEAVING;
        spdlog::info("Training: battle told to leave for the main menu");
    }
    beforeFrame = Native::GetNumFramesSimulated_FixedPoint(system)->integral;
    following = true;
    // Opening either overlay suspends recording; native pause frames are
    // also excluded by the before/after simulation counter check.
    const bool menu = session.GetView().ready && (sf4e::Overlay::CapturesMenuInput() || sf4e::Pad::MenuInputBlocked());
    const InputPlan plan = PlanInput(session.GetView(), menu);
    sampling = plan.sample;
    if (plan.read) {
        Pad* pad = Pad::staticMethods.GetSingleton();
        Frame physical;
        for (int side = 0; side < 2 && !menu; ++side) {
            physical[side].mapped = (pad->*Pad::publicMethods.GetButtons_MappedOn)(side);
            physical[side].raw = (pad->*Pad::publicMethods.GetButtons_RawOn)(side);
        }
        // A second side the game holds no assignment for reads every device, so
        // its presses are Player 1's: after a match as Player 2 it still names the
        // pad given back to Player 1. Two players on one keyboard are both assigned.
        if (!menu && !sf4e::input::SideAssigned(1)) physical[1] = Input{};
        output = session.Prepare(physical);
        commitInput = true;
    }
    overriding = plan.override;
    FollowDummy(system, plan.suppliesDummy);
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
        // A focus or a dash, by the category of the command move the Skill plays,
        // as the game tells its own (docs/design/FOCUS_CANCEL_MARKER.md).
        const unsigned char* command = sample.status == Actor::AS_SKILL ? (actor->*Actor::publicMethods.GetCommandMove)() : nullptr;
        std::uint32_t category = 0;
        if (command) std::memcpy(&category, command + 0x28, sizeof category);
        sample.actionClass = ResolveActionClass(true, sample.status, command != nullptr, category);
        (actor->*Actor::publicMethods.GetDamage)(&value);
        sample.damage = Dimps::Math::FixedToFloat(&value);
        (actor->*Actor::publicMethods.GetComboDamage)(&value);
        sample.comboDamage = Dimps::Math::FixedToFloat(&value);
        (actor->*Actor::publicMethods.GetVitalityAmt_FixedPoint)(&value);
        sample.health = Dimps::Math::FixedToFloat(&value);
        sample.untilActionable = (actor->*Actor::publicMethods.GetFramesUntilActionable)();
        sample.valid = true;
    }
    return fighters;
}
// The boxes and the camera as the game holds them now. Game thread, in a fight.
BoxView ReadBoxes(Native* system) {
    using Actor = Dimps::Game::Battle::Chara::Actor;
    using Unit = Dimps::Game::Battle::Chara::Unit;
    using Camera = Dimps::Game::Battle::Camera::Unit;
    using Effects = Dimps::Game::Battle::Effect::Unit;
    BoxView result;
    Unit* unit = (system->*Native::publicMethods.GetCharaUnit)();
    Camera* camera = (Camera*)(system->*Native::publicMethods.GetUnitByIndex)(Native::U_CAMERA);
    Effects* effects = (Effects*)(system->*Native::publicMethods.GetUnitByIndex)(Native::U_EFFECT);
    if (!unit || !camera) return result;
    std::memcpy(result.view, Camera::GetViewMatrix(camera), sizeof(result.view));
    std::memcpy(result.projection, Camera::GetProjectionMatrix(camera), sizeof(result.projection));
    Box box{};
    for (unsigned side = 0; side < 2; ++side) {
        Actor* actor = (unit->*Unit::publicMethods.GetActorByIndex)(side);
        if (!actor) continue;
        const auto add = [&](const unsigned char* record, BoxKind kind) { if (ReadBox(record, kind, box)) { box.side = static_cast<std::uint8_t>(side); result.boxes.push_back(box); } };
        // The boxes the game tests this frame, as its masks say.
        const unsigned strike = Actor::GetBodyMask(actor, Actor::BodyList::Strike), throwable = Actor::GetBodyMask(actor, Actor::BodyList::Throw);
        for (unsigned part = 0; part < Actor::BODY_STRIKE_COUNT; ++part)
            if (strike >> part & 1) add(Actor::GetBodyBox(actor, Actor::BodyList::Strike, part), BoxKind::Hurt);
        for (unsigned part = 0; part < Actor::BODY_THROW_COUNT; ++part)
            if (throwable >> part & 1) add(Actor::GetBodyBox(actor, Actor::BodyList::Throw, part), BoxKind::Throwable);
        const auto attack = [&](const void* from, unsigned list, BoxKind kind) {
            unsigned count = 0;
            const unsigned char* record = Actor::GetAttackBoxes(from, list, count);
            // The chain can close on itself, so the count ends it.
            for (unsigned n = 0; record && n < (std::min)(count, 32u); ++n, record = *(const unsigned char* const*)(record + Actor::BOX_NEXT))
                if (record[Actor::BOX_STATE] == Actor::BOX_LIVE) add(record, kind);
        };
        attack(actor, Actor::BOXES_HIT, BoxKind::Hit);
        attack(actor, Actor::BOXES_THROW, BoxKind::Throw);
        if (const void* effect = effects ? Effects::GetFirstEffect(effects, side) : nullptr) attack(effect, Effects::BOXES_HIT, BoxKind::Hit);
    }
    result.valid = true;
    return result;
}
void AfterUpdate(Native* system) {
    overriding = false;
    // The native integral field wraps at 16 bits. Count only one accepted
    // step; native resets/time jumps must not become recorded input frames.
    const int afterFrame = following ? Native::GetNumFramesSimulated_FixedPoint(system)->integral : beforeFrame;
    const auto delta = static_cast<std::uint16_t>(afterFrame - beforeFrame);
    // The input this update read is the next simulated frame's.
    if (delta != 0) dummyControl.Simulated();
    if (sampling && delta == 1) {
        std::array<FighterSample, 2> fighters = ReadFighters(system, true);
        if (commitInput) for (int side = 0; side < 2; ++side) fighters[side].input = output[side].raw & FightButtons;
        // The dummy as its stun is timed, for a reply's wait now and its decision below.
        const DummySeen seen = dummyWatch.Observe(fighters[1], fighters[0].action);
        // A playback's waiting frames read the fight: the fighter playing it
        // in a neutral state, and a hit on the other one landing this frame.
        {
            const int side = session.GetView().playbackSide;
            const auto& own = fighters[side], & other = fighters[1 - side], & before = playbackSeen[1 - side];
            const bool hit = other.valid && ((ClassifyStatus(other.status) == Phase::Hit && ClassifyStatus(before.status) != Phase::Hit) ||
                other.comboDamage > before.comboDamage);
            // How soon the script says the fighter is free again, so a link's
            // press can land on that frame: its interruptible frame, else its end.
            const int boundary = MoveRecoveryEnd(own);
            const int until = own.valid && boundary > own.actionFrame ? static_cast<int>(std::ceil(boundary - own.actionFrame)) : -1;
            // A reply's leading hold gives way as its first move is due.
            session.Observe(own.valid && ClassifyStatus(own.status) == Phase::Neutral, hit, until,
                session.Replying() && ReplyFirstMoveDue(session.Schedule(), seen));
            playbackSeen = fighters;
        }
        // Recorded for the facing the input was prepared with, then the new positions.
        if (commitInput) session.Commit(output);
        FollowPositions(system);
        ApplyMeterSettings();
        meter.Observe(Native::GetNumFramesSimulated_FixedPoint(system)->integral, fighters);
        // The dummy is free again: it may reply, and it may change its stance.
        // Its stun is timed, so the reply's motion can go in before the free
        // frame and its button land on it; a stun not seen before is replied
        // to as it ends.
        {
            int* options = DummyOptions();
            // Typed or recorded, the reply is played for the way the dummy
            // faces on each frame (Session::Prepare), from the next update.
            if (replies.Observe(dummyPlan, seen, session, dummyRandom()).none)
                spdlog::info("Training: no reply, no recording that is on can be one");
            if (seen.freed && dummyPlan.varyStance && options) {
                const int action = options[Manager::OPT_ACTION];
                if (action == 0 || action == 1) dummyControl.Set(options[Manager::OPT_ACTION], dummyRandom() & 1);
            }
            // Where the reply's button met the dummy, once per reply, to check timing 0 in the game:
            // a reversal's move starts on the free frame, the update its button is acted on
            // (ReplyTiming::Acted), so both differences are timing.
            ReplyTiming timing;
            const bool pressed = playing.reply && commitInput && (output[1].raw & AttackButtons);
            if (replyTrace.Observe(session.Replies(), session.Replying(), pressed, seen, fighters[1].valid && fighters[1].status == ActorStatus::Skill,
                    session.Schedule().timing, timing))
                spdlog::info("Training: reply trace, frames after the reply was decided: button sent {}, dummy free {}, in its move {}; "
                    "Lower input lag {}, timing {}; move {} after free, {} after the button was acted on",
                    SeenAt(timing.sent), SeenAt(timing.freed), SeenAt(timing.move), sf4e::InputLag::BattleInputLowered() ? "on" : "off", timing.timing,
                    FramesBetween(timing.freed, timing.move), FramesBetween(timing.Acted(), timing.move));
        }
        if (!capture) capture = new TrainingCapture();
        capture->Record(Native::GetNumFramesSimulated_FixedPoint(system)->integral, fighters, meter.View());
    } else if (sampling && delta != 0) {
        meter.Reset(); ForgetStun(dummyWatch, session, replies); replyTrace.Reset();
        ++gapResets;
    }
    if (sampling && delta == 1) unfedUpdates = unfedNotReady = unfedMultiFrame = 0;
    else if (delta != 0 && session.GetView().available) {
        ++unfedUpdates; ++(sampling ? unfedMultiFrame : unfedNotReady);
        if (unfedUpdates == MeterUnfedLogUpdates) {
            const auto& view = meter.View();
            spdlog::warn("Training: frame meter not fed for {} updates that simulated frames ({} not ready, {} more than one frame, "
                "last step {}); held={} frames={} resets this battle={}",
                unfedUpdates, unfedNotReady, unfedMultiFrame, delta, view.frozen, view.frames.size(), gapResets);
        }
    }
    sampling = false; following = false;
    const View view = session.GetView();
    BoxView boxes;
    if (view.available && view.ready && BoxesWanted()) boxes = ReadBoxes(system);
    std::lock_guard<std::mutex> lock(mutex); published = view; published.meter = meter.View(); published.boxes = std::move(boxes);
    if (published.available) published.dummy = ReadDummyState(published.dummy);
    published.exportId = exportId; published.exportedSlot = exportedSlot; published.exported = exported;
    published.leavingIn = leaving.Left(); published.leavingCall = leaving.Call();
    published.acks=acks;
}
void SetMatchPractice(bool enabled) { matchPractice = enabled; }
bool MatchPracticeActive() { return matchPractice; }
unsigned ClearReservedInputBits(unsigned raw) {
    return matchPractice ? raw & ~ReservedInputBits : raw;
}
#ifdef SF4E_SELFTEST
void AllowOfflineVersusForTest() { versusForTest = true; }
void ReadHitBoxesForTest() { boxesForTest = true; }
#endif
void WatchMatches(bool enabled) { watching = enabled; }
void SetMeterHistory(int frames, bool continuous) { meterHistory = frames; meterContinuous = continuous; }
void ShowHitBoxes(bool shown) { hitBoxesShown = shown; }
void ObserveMatch(Native* system, int stateFrame, int lastConfirmedInput, const unsigned* inputs) {
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
    auto fighters = ReadFighters(system, false);
    ApplyMeterSettings();
    if (inputs) for (int side = 0; side < 2; ++side) fighters[side].input = inputs[side] & FightButtons;
    // A spectator plays confirmed inputs only and has no save frame to name one by.
    if (stateFrame <= 0) meter.Observe(Native::GetNumFramesSimulated_FixedPoint(system)->integral, fighters);
    else {
        confirmed.Capture(stateFrame, fighters);
        int frame = 0; std::array<FighterSample, 2> next;
        if (!confirmed.Next(lastConfirmedInput, frame, next)) return;
        do meter.Observe(frame, next); while (confirmed.Next(lastConfirmedInput, frame, next));
    }
    std::lock_guard<std::mutex> lock(mutex);
    published.meter = meter.View(); published.watching = matchShown = true; published.replayInputs = inputs != nullptr;
}
void StopCapture() { delete capture; capture = nullptr; }
void CloseBattle() {
    if (session.GetView().available)
        spdlog::info("Training: frame meter reset {} times on multi-frame updates this battle", gapResets);
    gapResets = 0; unfedUpdates = unfedNotReady = unfedMultiFrame = 0; leaving.Reset();
    overriding = false; sampling = false;
    if (checkpoint.used) Battle::SaveState::Free(&checkpoint);
    if (dummyControl.Release()) if (Native* system = Native::staticMethods.GetSingleton()) ReturnDummyControl(system);
    playing = {};
    // The slots stay with the dummy fighter for its next battle, a recording under way among them.
    if (session.GetView().available) banks.Detach(session.TakeBank());
    session.Reset(); meter.Reset(); confirmed.Reset(); matchShown = false;
    matchPractice = false; dummyWatch.Reset(); replies.Reset(); replyTrace.Reset(); playbackSeen = {};
    std::lock_guard<std::mutex> lock(mutex); commands.Clear(); published = session.GetView();
}
} }
