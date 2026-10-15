#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <tuple>
#include <vector>
#include <string>
#include "FrameMeter.hxx"

namespace sf4e { namespace training {
constexpr int SlotCount = 8;
// One more slot, never selectable, holds the dummy's typed reply.
constexpr int ReplySlot = SlotCount;
constexpr int MaxFrames = 60 * 120;
constexpr int HistoryRows = 12;
constexpr unsigned FightButtons = 0xcff; // Directions and six attacks; excludes menu buttons.
constexpr unsigned AttackButtons = 0xcf0;
// wait: a loaded frame that repeats, buttons held, until the fighter can act
// again (1) or a hit lands (2), so typed moves take their timing from the
// fight; offset: frames after the cue the press lands on. A free frame is
// predicted from the fighter's script, so 0 is that frame itself and a
// negative offset is before it; a hit is pressed on the frame after it is seen.
// A reply's leading hold (3) repeats until the reply's first move is due
// (ReplyFirstMoveDue); elsewhere it is met at once.
struct Input { unsigned mapped = 0, raw = 0; unsigned char wait = 0; signed char offset = 0; };
constexpr int MinOffset = -120, MaxOffset = 120;
// A slot holds its frames as played by a fighter facing right, so a move
// recorded on one side comes out the same on the other: Left and Right are
// swapped for a fighter facing left, in both words, per played frame.
constexpr unsigned LeftBit = 4, RightBit = 8;
inline unsigned MirrorDirections(unsigned bits) {
    return (bits & ~(LeftBit | RightBit)) | (bits & LeftBit ? RightBit : 0) | (bits & RightBit ? LeftBit : 0);
}
inline Input Mirror(Input input) { input.mapped = MirrorDirections(input.mapped); input.raw = MirrorDirections(input.raw); return input; }
// Between the slot's facing and a fighter's: the same swap both ways.
inline Input ForFacing(const Input& input, bool facingRight) { return facingRight ? input : Mirror(input); }
constexpr unsigned char WaitActionable = 1, WaitHit = 2, WaitReply = 3;
// How long a waiting frame may wait before playback gives up on its
// condition: a whole recovery, or the few frames a buffered press can wait
// for its hit before the motion goes stale. A reply's leading hold has no
// such limit: the stun it waits out always ends, and a new one drops the reply.
constexpr int MaxWaitFrames = 90, MaxWaitHitFrames = 15;
using Frame = std::array<Input, 2>;
struct InputRun { unsigned buttons = 0; unsigned frames = 0; };
enum class Mode { Idle, Recording, Playback };
enum class Action { Select, Record, Play, Stop, Clear, Loop, Save, Restore, ClearHistory, Load, ExportSlot, DummyState, Place, Mirror, DummyPlan };
// The dummy's behaviour as the game's Training menu sets it; each value is
// the menu's choice index and -1 leaves that setting as it is. action: stand,
// crouch, jump, cpu, human (Player 2's pad), then the menu's own record and
// playback. guard: no block, after first hit, all, random.
// quickStand: quick, normal, delayed, random. counterHit: off, on, random.
// stun: normal, constant, none.
// super, revenge: the gauge settings as the menu stores them, 0 normal, 5
// max at round start, 7 infinite, 8 refill.
struct DummyState { int action = -1, guard = -1, quickStand = -1, counterHit = -1, stun = -1, super = -1, revenge = -1; };
// Updates from the one Ember's input for the dummy is read in to the one the
// dummy acts on it, whether Lower input lag (InputLagEdit.hxx) is on or not.
// Measured in the game, not derived: a reply's button read in one update
// started the dummy's move in the next with the setting off, and in the next
// as well with it on, though the edit at 0x5CFBB6 stores both sides' input
// under the frame being read.
constexpr int DummyInputLag = 1;
// When the dummy plays from Player 2's pad input, which Ember overrides while
// it drives Player 2 (DrivesDummy). The game plays the dummy by its own logic,
// which follows the Training menu's Action and Guard, and plays a side from
// its pad only when the side's control says so; Ember gives Player 2 that
// control for exactly the updates that read or act on Ember's input and
// the game's own back after, so the menu's settings, blocking among them,
// hold everywhere else and the player never sets anything. The game reads a
// side's pad only while its control says so (0x599410, any other control
// reads nothing), and the input reaches the fighter the next simulated frame
// (DummyInputLag), where the fighter plays from it only while the control
// still says so (0x5996F0, 0x59A6A0). So the pad control starts with the
// first update Ember supplies and ends with the simulated one after the
// last; an update the battle does not simulate moves nothing on.
// The Action found when Ember began to drive Player 2 stays the player's
// while Ember drives it or the pad control lasts: a different one found there
// was picked in the pause menu, which ends both (Reconcile) so that choice
// stands. Ember changes it only through Set. A reset breaks the input's
// history but not that: it is reconciled first. Only the game thread owns
// this.
class DummyControl {
public:
    enum class Step { None, Begin, Keep, End };
    // At the start of each update, before its commands: whether the pause
    // menu changed the action under Ember. The pad control and its history
    // end then too.
    bool Reconcile(int action) {
        if (!owned_ || action == action_) return false;
        owned_ = false; Rewind(); return true;
    }
    // Once the update's input is planned. supplies: Ember plays Player 2's
    // input in it (InputPlan). action: the Training menu's Action now.
    Step Follow(bool supplies, int action) {
        static_assert(DummyInputLag == 1, "the pad control covers one simulated update after the last input");
        supplies_ = supplies;
        const bool pad = supplies || supplied_;
        if (!owned_) action_ = action;
        owned_ = supplies || pad;
        if (pad == pad_) return pad ? Step::Keep : Step::None;
        pad_ = pad;
        return pad ? Step::Begin : Step::End;
    }
    // After an update the battle simulated: its input is the next one's to act on.
    void Simulated() { supplied_ = supplies_; }
    void Set(int& native, int value) { native = value; action_ = value; }
    // A checkpoint put back a timeline whose delayed input was never Ember's.
    // Whether the pad control was on.
    bool Rewind() { const bool was = pad_; pad_ = supplied_ = supplies_ = false; return was; }
    // The battle closes. Whether the pad control was on.
    bool Release() { owned_ = false; return Rewind(); }
private:
    bool owned_ = false, pad_ = false, supplied_ = false, supplies_ = false;
    int action_ = -1;
};
constexpr int GaugeValues[] = {0, 5, 7, 8};
inline bool operator==(const DummyState& a, const DummyState& b) {
    return a.action == b.action && a.guard == b.guard && a.quickStand == b.quickStand && a.counterHit == b.counterHit && a.stun == b.stun &&
        a.super == b.super && a.revenge == b.revenge;
}
// Whether every value is -1 or a choice the menu offers.
inline bool ValidDummyState(const DummyState& s) {
    return s.action >= -1 && s.action <= 6 && s.guard >= -1 && s.guard <= 3 && s.quickStand >= -1 && s.quickStand <= 3 &&
        s.counterHit >= -1 && s.counterHit <= 2 && s.stun >= -1 && s.stun <= 2 &&
        (s.super == -1 || s.super == 0 || s.super == 5 || s.super == 7 || s.super == 8) &&
        (s.revenge == -1 || s.revenge == 0 || s.revenge == 5 || s.revenge == 7 || s.revenge == 8);
}
// What the dummy does by itself once it is free again. when: 0 nothing, 1
// after being hit, 2 after blocking, 3 on getting up, 4 any
// of them. moves: the reply as made-up input, held as a slot holds it (for
// the dummy facing right) and played for the way it faces on each frame; when
// empty it replies with the recording in slot. chance:
// percent of the times it replies. timing: frames the reply's first attack
// button lands after the dummy's first free frame. varyStance: it stands or
// crouches at random each time. The typed moves' schedule, as synthesis
// gives it (Compiled), with no typed moves 0, -1 and -1: offset, the first
// move's own ("5MK@+3"), added to timing; anchor, the frame of moves its
// input takes effect on; hold, the frame of its leading hold, -1 for none.
struct DummyPlan {
    int when = 0, slot = 0, chance = 100, timing = 0; bool varyStance = false; std::vector<Input> moves;
    int offset = 0, anchor = -1, hold = -1;
};
constexpr int MaxReplyTiming = 5;
inline bool ValidDummyPlan(const DummyPlan& plan) {
    const int frames = static_cast<int>(plan.moves.size());
    const bool schedule = frames ? plan.offset >= MinOffset && plan.offset <= MaxOffset && plan.anchor >= 0 && plan.anchor < frames &&
        plan.hold < plan.anchor && (plan.hold < 0 || plan.moves[plan.hold].wait == WaitReply) : !plan.offset && plan.anchor < 0 && plan.hold < 0;
    return plan.when >= 0 && plan.when <= 4 && plan.slot >= 0 && plan.slot < SlotCount && plan.chance >= 0 && plan.chance <= 100 &&
        plan.timing >= -MaxReplyTiming && plan.timing <= MaxReplyTiming && frames <= MaxFrames && schedule;
}
// A reply starts on its first pressed frame; -1 when it presses nothing.
inline int ReplyStart(const std::vector<Input>& frames) {
    const auto first = std::find_if(frames.begin(), frames.end(), [](const Input& frame) { return (frame.raw & FightButtons) != 0; });
    return first == frames.end() ? -1 : static_cast<int>(first - frames.begin());
}
// When a reply's first move lands, in frames of the reply as Session plays
// it: start, its first played frame (ReplyStart); hold, the frame its
// leading hold repeats on, -1 for none ("[1] > 3HP"); anchor, the frame its
// first move's input takes effect on; timing, frames after the dummy's free
// frame that frame is acted on. It goes with the frames it was made for
// (Session::Reply), whatever the plan becomes after.
struct ReplySchedule { int start = -1, hold = -1, anchor = -1, timing = 0; };
// Typed moves carry theirs from synthesis (DummyPlan). A recording's comes
// from its frames: it lands from its first attack button, and has no hold.
inline ReplySchedule RecordingSchedule(const std::vector<Input>& frames, int timing) {
    ReplySchedule schedule; schedule.start = ReplyStart(frames); schedule.timing = timing;
    const auto press = std::find_if(frames.begin(), frames.end(), [](const Input& frame) { return (frame.raw & AttackButtons) != 0; });
    schedule.anchor = press == frames.end() ? schedule.start : static_cast<int>(press - frames.begin());
    return schedule;
}
// The schedule of the reply a plan plays: its typed moves', else its slot's recording's.
inline ReplySchedule PlanSchedule(const DummyPlan& plan, const std::vector<Input>& slot) {
    if (plan.moves.empty()) return RecordingSchedule(slot, plan.timing);
    ReplySchedule schedule; schedule.start = ReplyStart(plan.moves); schedule.hold = plan.hold; schedule.anchor = plan.anchor;
    schedule.timing = plan.timing + plan.offset;
    return schedule;
}
// Frames a reply plays before its first move takes effect, after its leading
// hold if it has one: holds and waits leading into the move and its motion,
// which are done while the dummy is still held so the move meets the free frame.
inline int ReplyLead(const ReplySchedule& schedule) {
    return schedule.anchor < 0 ? 0 : schedule.anchor - (schedule.hold >= 0 ? schedule.hold + 1 : schedule.start);
}
// freed: on the frame the dummy is free again, what it came out of, numbered
// as DummyPlan::when (1 hit, 2 block, 3 knockdown; a hit that ends in a
// knockdown is a knockdown). held: the same while it is still in it. until:
// frames from now to that free frame, -1 while not known. free: frames since
// that free frame (0 on it), -1 while held or before any stun; out: what it
// came out of while free counts. stretch: counts the hits, blocks and
// reactions, so a change says the wait began again.
struct DummySeen { int freed = 0, held = 0, until = -1, free = -1, out = 0; unsigned stretch = 0; };
// What the dummy's status holds it in, numbered as DummyPlan::when: 1 hit
// or dizzy, 2 blockstun, 3 knocked down or getting up, 0 free. Not the
// frame meter's grouping: the game sets GuardStand and GuardCrouch only to
// hold the dummy back while an attack is near, with nothing landed, and the
// dummy can act from them; blockstun is DamageGuard alone. -1: neither, as
// a throw's sequence or a frame not read, which neither starts nor ends a stun.
inline int DummyHold(const FighterSample& dummy) {
    using S = ActorStatus;
    if (!dummy.valid) return -1;
    switch (dummy.status) {
    case S::Damage: case S::DamageBlow: case S::Stun: return 1;
    case S::DamageGuard: return 2;
    case S::Bound: case S::Down: case S::Rise: return 3;
    default: return ClassifyStatus(dummy.status) == Phase::Unknown ? -1 : 0;
    }
}
// Fed the dummy once per simulated frame. The game does not say how long a
// stun lasts, so each one is timed the first time it is seen, from its last
// hit or change of reaction to the free frame, and known from then on.
// Limitation: one length per attacking move and reaction; a counter hit's
// extra frames replace it until the plain hit is seen again. Key on the hit's
// own data if the two ever have to be told apart.
class DummyWatch {
public:
    // attacker: the other fighter's action id.
    DummySeen Observe(const FighterSample& dummy, int attacker) {
        DummySeen seen; seen.stretch = stretch_;
        const int hold = DummyHold(dummy), held = (std::max)(0, hold);
        if (held) {
            if (!from_ || dummy.status != last_.status || dummy.action != last_.action || dummy.actionFrame < last_.actionFrame ||
                dummy.comboDamage > last_.comboDamage) {
                // Getting up takes the same time whatever knocked the dummy down.
                ++stretch_; elapsed_ = 0; key_ = Key(dummy.status, dummy.action, held == 3 ? -1 : attacker);
            } else ++elapsed_;
            from_ = held; last_ = dummy;
            const auto known = learned_.find(key_);
            seen.held = held; seen.stretch = stretch_;
            if (known != learned_.end()) seen.until = (std::max)(0, known->second - elapsed_);
            free_ = -1;
        } else if (from_ && hold < 0) ++elapsed_;
        else if (from_) {
            if (learned_.size() >= 4096) learned_.clear();
            learned_[key_] = elapsed_ + 1; seen.freed = out_ = from_; from_ = 0; free_ = 0;
        } else if (free_ >= 0) ++free_;
        if (!seen.held && free_ >= 0) { seen.free = free_; seen.out = out_; }
        return seen;
    }
    // The frames stopped being consecutive; what was learned stays.
    void Reset() { from_ = 0; free_ = -1; }
private:
    using Key = std::tuple<unsigned, int, int>;
    std::map<Key, int> learned_;
    Key key_;
    FighterSample last_;
    int from_ = 0, elapsed_ = 0, free_ = -1, out_ = 0;
    unsigned stretch_ = 0;
};
// cause: DummySeen::held, else out while the dummy is free. roll: any random number; it decides the chance.
inline bool DummyReplies(const DummyPlan& plan, int cause, unsigned roll) {
    return cause && (plan.when == 4 || plan.when == cause) && roll % 100 < static_cast<unsigned>(plan.chance);
}
// Whether a move played from the next update should start after this
// frame's observation: its first attack button, acted on lead frames in (its
// lead plus DummyInputLag), then meets the free frame plus timing. The free
// frame is ahead while a timed stun counts down and behind once the dummy is
// free, so a positive timing waits past it; a stun not timed is replied to as
// it ends, or later by timing.
inline bool ReplyDue(const DummySeen& seen, int lead, int timing) {
    if (seen.held) return seen.until >= 0 && seen.until <= lead + 1 - timing;
    return seen.free >= 0 && -seen.free <= lead + 1 - timing;
}
// Whether a reply's first move is due by its schedule.
inline bool ReplyFirstMoveDue(const ReplySchedule& schedule, const DummySeen& seen) {
    return ReplyDue(seen, ReplyLead(schedule) + DummyInputLag, schedule.timing);
}
// Whether a reply should begin after this frame's observation: one holding
// first begins with the stun, any other when its first move is due.
inline bool ReplyBegins(const ReplySchedule& schedule, const DummySeen& seen) {
    return (seen.held && schedule.hold >= 0) || ReplyFirstMoveDue(schedule, seen);
}
// Where one reply's button met the dummy, in frames after the one the reply
// was decided in, NotSeen where not seen: the frame its first attack button
// was sent in (acted on DummyInputLag frames later), the dummy's first free
// frame, before the decision (negative) when a later timing waited past it,
// and the first frame of its move. A stun ends and a move starts in the same
// game frame, so a button that meets the free frame shows the last two the same.
// timing: the reply's own (ReplySchedule), as it began.
struct ReplyTiming {
    static constexpr int NotSeen = -1000000;
    int sent = NotSeen, freed = NotSeen, move = NotSeen, timing = 0;
    int Acted() const { return sent == NotSeen ? NotSeen : sent + DummyInputLag; }
};
// For the trace: a frame, or "none" when it was not seen.
inline std::string SeenAt(int frame) { return frame == ReplyTiming::NotSeen ? "none" : std::to_string(frame); }
// For the trace: frames from one observation to a later one, "none" when
// either was not seen.
inline std::string FramesBetween(int from, int to) {
    return from == ReplyTiming::NotSeen || to == ReplyTiming::NotSeen ? "none" : std::to_string(to - from);
}
// Follows each reply once, for the log, from the frame it is decided in
// (Session::Replies changes) over consecutive frames, counting them itself.
// It is done once all three are seen, or Grace frames after the reply's
// last; a new stun drops it, and so does Reset, for a reset, a gap in the
// frames or the battle closing. A reply dropped or done is never traced again.
class ReplyTrace {
public:
    static constexpr int Grace = 30;
    // Each frame DummyWatch sees, after the reply's decision. reply:
    // Session::Replies; replying: Session::Replying; pressed: the frame
    // played the reply's attack button; moving: the dummy is in a move
    // (Skill); timing: the reply's, kept from the frame it begins. True when
    // done, with out set.
    bool Observe(std::uint64_t reply, bool replying, bool pressed, const DummySeen& seen, bool moving, int timing, ReplyTiming& out) {
        const bool begins = reply != reply_;
        if (begins) {
            reply_ = reply; on_ = true; timing_ = {}; timing_.timing = timing; stretch_ = seen.stretch; frame_ = after_ = 0;
            // Decided after the free frame, by a later timing.
            if (seen.free > 0) timing_.freed = -seen.free;
        }
        else if (on_) ++frame_;
        if (!on_) return false;
        if (seen.held && seen.stretch != stretch_) { Reset(); return false; }
        // What played in the frame a reply is decided in was not that reply: it plays from the next.
        if (pressed && !begins && timing_.sent == ReplyTiming::NotSeen) timing_.sent = frame_;
        if (seen.freed && timing_.freed == ReplyTiming::NotSeen) timing_.freed = frame_;
        if (moving && timing_.move == ReplyTiming::NotSeen) timing_.move = frame_;
        if (replying) after_ = 0;
        const bool seenAll = timing_.sent != ReplyTiming::NotSeen && timing_.freed != ReplyTiming::NotSeen && timing_.move != ReplyTiming::NotSeen;
        if (!seenAll && after_++ < Grace) return false;
        out = timing_; Reset(); return true;
    }
    void Reset() { on_ = false; }
private:
    std::uint64_t reply_ = 0;
    bool on_ = false;
    ReplyTiming timing_;
    unsigned stretch_ = 0;
    int frame_ = 0, after_ = 0;
};
// Each action reads only the fields named for it; the rest are ignored.
struct Command {
    Action action = Action::Stop; std::uint64_t generation = 0; std::uint64_t requestId = 0;
    // Select: the slot to choose.
    int slot = 0;
    // Loop: whether playback starts again at its end.
    bool loop = false;
    // Load: made-up input for the selected slot, as a slot holds it (for a
    // fighter facing right), and the side (0 or 1) that plays it back.
    std::vector<Input> frames; int side = 1;
    // DummyState: the settings to change.
    DummyState dummy;
    // Place: where to put Player 1 and Player 2 (x), moved by the runtime (FighterPlacement.hxx).
    float place[2] = {0, 0};
    // DummyPlan: the whole plan, replacing the one before.
    DummyPlan plan;
};
// The results of the last few commands sent with a request id. Several can
// be applied in one game frame, and the overlay may read only the frame
// after, so each is kept until newer ones push it out; a command without an
// id leaves them as they are.
class Acks {
public:
    void Note(std::uint64_t id, bool accepted) {
        if (!id) return;
        std::rotate(acks_.begin(), acks_.begin() + 1, acks_.end()); acks_.back() = {id, accepted};
    }
    // Whether the command with this id has been applied, and if so its result.
    bool Find(std::uint64_t id, bool& accepted) const {
        for (const auto& ack : acks_) if (id && ack.id == id) { accepted = ack.accepted; return true; }
        return false;
    }
private:
    struct Ack { std::uint64_t id = 0; bool accepted = false; };
    std::array<Ack, 8> acks_{};
};
// The call back from Training as the room's call owner holds it for this
// battle, published each tick (TrainingRuntime.hxx: SetCall) and read before
// every countdown tick: the call's serial (0: no call stands), the battle it
// was sent to, whether the player chose go now for it, and the announcer's
// volume in percent, 0 for none. It is state, not a message: nothing about
// the call is lost when the Training command queue is full.
struct CallControl {
    std::uint64_t call = 0, generation = 0;
    bool hurry = false;
    int volume = 0;
};
// The frames a called player's battle has left before it goes back to the
// main menu. The count follows the call that stands for this battle, once
// per battle update, before it ticks: a call it has not counted for starts a
// whole count of its own (a new opponent or another room starts over, without
// the old call's go now); the call ending stops it, unless the battle was
// already told to leave; the call's go now shortens it to the next tick that
// may leave, once. So a battle leaves by one path, once, for the call that
// stands, whichever ended the wait.
class LeaveCountdown {
public:
    // control: what was published; battle: this battle's generation; frames:
    // the count a new call starts with. True when a new count started, for
    // the caller to play the call's announcer.
    bool Follow(const CallControl& control, std::uint64_t battle, int frames) {
        const std::uint64_t call = control.generation == battle ? control.call : 0;
        bool started = false;
        if (!call) left_ = 0;
        else if (call != call_ && frames > 0) { left_ = frames; started = true; }
        call_ = call;
        if (call && control.hurry && left_ > 1) left_ = 1;
        return started;
    }
    // Once per battle update. fighting: the fight is running, the only time
    // the pause menu's exit could be used too; the last frame waits for it.
    // True on the one update the battle is to be told to leave.
    bool Tick(bool fighting) { return left_ && (left_ > 1 || fighting) && !--left_; }
    int Left() const { return left_; }
    // The call the count is for; 0 when none is counting.
    std::uint64_t Call() const { return left_ ? call_ : 0; }
    void Reset() { left_ = 0; call_ = 0; }
private:
    int left_ = 0;
    // The call last followed, counting or already told to leave.
    std::uint64_t call_ = 0;
};
struct View {
    bool available = false, ready = false, checkpoint = false, loop = true;
    // A rollback match whose frame meter is shown: the meter is filled,
    // nothing else, and no command is taken.
    bool watching = false;
    std::uint64_t generation = 0;
    // Where Player 1 and Player 2 stand (x), as the adapter reads them each frame.
    float x[2] = {0, 0};
    // The battle has been told to leave for the main menu: a challenger is
    // waiting. Frames left before it goes, 0 when it was not.
    int leavingIn = 0;
    // The call the countdown is for (LeaveCountdown::Call), 0 when none.
    std::uint64_t leavingCall = 0;
    Mode mode = Mode::Idle;
    int selected = 0, cursor = 0, playbackSide = 1;
    std::array<int, SlotCount + 1> lengths{};
    std::array<std::deque<InputRun>, 2> history;
    MeterView meter;
    // ExportSlot: the selected slot's frames, handed over once per request.
    std::uint64_t exportId = 0; int exportedSlot = -1;
    std::vector<Input> exported;
    Acks acks;
    // The dummy's current settings, read from the game every frame; all -1
    // until the adapter has read them.
    DummyState dummy;
};

// Whether Ember plays Player 2 this frame: a recording, or a playback (a
// reply among them) on its side. The dummy plays from the pad then (DummyControl).
inline bool DrivesDummy(const View& view) {
    return view.mode == Mode::Recording || (view.mode == Mode::Playback && view.playbackSide == 1);
}
// What one update does with the fighters' input, decided once for it.
// sample: the fight runs, so the update may be a recorded frame. read: Ember
// prepares the input and commits it after. override: the fighters read
// Ember's input. suppliesDummy: Player 2's is Ember's (DummyControl).
struct InputPlan { bool sample = false, read = false, override = false, suppliesDummy = false; };
// menu: Ember's controls or the game hold the pad. Playback needs no pad, so
// it goes on under the open controls, where it can be watched; recording
// waits for the pad to be free.
inline InputPlan PlanInput(const View& view, bool menu) {
    InputPlan plan;
    if (!view.ready) return plan;
    plan.sample = true;
    if (menu && view.mode != Mode::Playback) return plan;
    plan.read = true;
    plan.override = view.mode != Mode::Idle;
    plan.suppliesDummy = plan.override && DrivesDummy(view);
    return plan;
}

// Owns only practice input data. The game adapter owns native save states.
// Prepare is repeatable; only Commit advances a recording or playback cursor.
class Session {
public:
    const View& GetView() const { return view_; }
    const std::vector<Input>& Slot(int slot) const { return slots_[slot]; }
    void Enter() { Reset(); view_.available = true; }
    void Reset() {
        const auto next = view_.generation + 1;
        view_ = View{}; view_.generation = next;
        for (auto& slot : slots_) slot.clear();
        actionable_ = hit_ = false; waited_ = waitedPast_ = 0; resume_ = -1; firstOnLeft_ = true;
    }
    void SetReady(bool ready) { view_.ready = ready; }
    // What the fight showed this frame, for the waiting frames of a playback:
    // whether the fighter is free now, and whether a hit landed on the other one.
    // untilActionable: frames until the fighter's script says it can act again, -1 when unknown.
    // replyDue: a reply's leading hold gives way (ReplyFirstMoveDue on Schedule).
    void Observe(bool actionable, bool hit, int untilActionable = -1, bool replyDue = false) {
        actionable_ = actionable; hit_ = hit_ || hit; untilActionable_ = untilActionable; replyDue_ = replyDue;
    }
    void SetCheckpoint(bool saved) { view_.checkpoint = saved; }
    // The fighters face each other, by where they stood after the last
    // update, or where a Place moved them (FighterPlacement.hxx); level, they keep
    // the sides they had. A slot plays, and is
    // recorded, for the facing the update's input was prepared with, so the
    // runtime sets the new positions after Commit.
    // Limitation: the game turns a fighter a few frames after the two pass
    // (after a jump lands), and this turns it at once.
    void SetPositions(float x0, float x1) {
        view_.x[0] = x0; view_.x[1] = x1;
        if (x0 != x1) firstOnLeft_ = x0 < x1;
    }
    // The fighter on the left faces right.
    bool FacingRight(int side) const { return (side == 0) == firstOnLeft_; }
    // Every command passes here first, and only here is its battle checked.
    // Place, DummyPlan and ExportSlot change nothing in the
    // session; accepted, the runtime carries them out.
    bool Apply(const Command& command) {
        if (!view_.available || command.generation != view_.generation) return false;
        switch (command.action) {
        case Action::Place: case Action::ExportSlot: return true;
        case Action::DummyPlan: return ValidDummyPlan(command.plan);
        case Action::Stop: Stop(); return true;
        case Action::Select:
            if (command.slot < 0 || command.slot >= SlotCount || view_.mode != Mode::Idle) return false;
            view_.selected = command.slot; return true;
        case Action::Loop: view_.loop = command.loop; return true;
        case Action::DummyState:
            if (!ValidDummyState(command.dummy)) return false;
            // The adapter writes the game; the view shows the request until
            // the next frame's read-back replaces it.
            if (command.dummy.action >= 0) view_.dummy.action = command.dummy.action;
            if (command.dummy.guard >= 0) view_.dummy.guard = command.dummy.guard;
            if (command.dummy.quickStand >= 0) view_.dummy.quickStand = command.dummy.quickStand;
            if (command.dummy.counterHit >= 0) view_.dummy.counterHit = command.dummy.counterHit;
            if (command.dummy.stun >= 0) view_.dummy.stun = command.dummy.stun;
            return true;
        case Action::ClearHistory: ClearHistory(); return true;
        default: break;
        }
        if (!view_.ready) return false;
        switch (command.action) {
        case Action::Record:
            Stop(); slots_[view_.selected].clear(); view_.lengths[view_.selected] = 0;
            view_.mode = Mode::Recording; view_.playbackSide = 1; return true;
        case Action::Load:
            if (view_.mode != Mode::Idle || command.frames.empty() || (command.side != 0 && command.side != 1)) return false;
            slots_[view_.selected].assign(command.frames.begin(), command.frames.begin() + (std::min)(command.frames.size(), std::size_t(MaxFrames)));
            view_.lengths[view_.selected] = static_cast<int>(slots_[view_.selected].size());
            view_.playbackSide = command.side; return true;
        case Action::Play:
            Stop();
            if (slots_[view_.selected].empty()) return false;
            view_.mode = Mode::Playback; return true;
        case Action::Clear:
            if (view_.mode != Mode::Idle) return false;
            slots_[view_.selected].clear(); view_.lengths[view_.selected] = 0; return true;
        // A loaded file that comes out the wrong way round: its Left and Right swapped.
        case Action::Mirror:
            if (view_.mode != Mode::Idle || slots_[view_.selected].empty()) return false;
            for (auto& frame : slots_[view_.selected]) frame = Mirror(frame);
            return true;
        case Action::Save: return true;
        case Action::Restore:
            if (!view_.checkpoint) return false;
            Rewind(); return true;
        default: return false;
        }
    }
    // The dummy's reply: the slot once on Player 2, from its first pressed
    // frame, after which the selected slot is the selected one again. Never
    // over a recording or another playback.
    // schedule: when its first move lands, kept with the reply (Schedule).
    bool Reply(int slot, const ReplySchedule& schedule) {
        if (!view_.ready || view_.mode != Mode::Idle || slot < 0 || slot > ReplySlot) return false;
        const int first = ReplyStart(slots_[slot]);
        if (first < 0) return false;
        resume_ = view_.selected; resumeSide_ = view_.playbackSide;
        view_.selected = slot; view_.playbackSide = 1; view_.mode = Mode::Playback;
        view_.cursor = first; ++replies_; schedule_ = schedule;
        return true;
    }
    // The same with made-up input, kept in a slot of its own.
    bool Reply(std::vector<Input> frames, const ReplySchedule& schedule) {
        if (!view_.ready || view_.mode != Mode::Idle || frames.size() > std::size_t(MaxFrames)) return false;
        slots_[ReplySlot] = std::move(frames); view_.lengths[ReplySlot] = static_cast<int>(slots_[ReplySlot].size());
        return Reply(ReplySlot, schedule);
    }
    bool Replying() const { return resume_ >= 0; }
    // The schedule of the reply playing, as it began.
    const ReplySchedule& Schedule() const { return schedule_; }
    // Replies started so far, never reset: each new reply changes it.
    std::uint64_t Replies() const { return replies_; }
    // The dummy was hit again, or what was seen of its stun went: the reply it had begun is dropped.
    void StopReply() { if (resume_ >= 0) Stop(); }
    Frame Prepare(Frame physical) const {
        if (!view_.available || !view_.ready) return physical;
        if (view_.mode == Mode::Recording) {
            physical[1] = physical[0]; physical[0] = Input{};
        } else if (view_.mode == Mode::Playback) {
            physical[view_.playbackSide] = ForFacing(slots_[view_.selected][view_.cursor], FacingRight(view_.playbackSide));
        }
        return physical;
    }
    void Commit(const Frame& output) {
        if (!view_.available || !view_.ready) return;
        for (int side = 0; side < 2; ++side) {
            const unsigned buttons = output[side].raw & FightButtons;
            auto& history = view_.history[side];
            if (!history.empty() && history.front().buttons == buttons) {
                if (history.front().frames < UINT32_MAX) ++history.front().frames;
            } else {
                history.push_front({buttons, 1});
                if (history.size() > HistoryRows) history.pop_back();
            }
        }
        if (view_.mode == Mode::Recording) {
            auto& slot = slots_[view_.selected];
            // The history above keeps the pad's own directions; the slot, the dummy's.
            slot.push_back(ForFacing(output[1], FacingRight(1)));
            view_.lengths[view_.selected] = static_cast<int>(slot.size());
            view_.cursor = static_cast<int>(slot.size());
            if (slot.size() == MaxFrames) Stop();
        } else if (view_.mode == Mode::Playback) {
            // A waiting frame repeats until its condition shows, or long enough.
            const auto& frame = slots_[view_.selected][view_.cursor];
            if (frame.wait == WaitReply) {
                // A reply's leading hold: until the runtime finds its first move due.
                if (resume_ >= 0 && !replyDue_) return;
            } else {
                // The cue, then the offset's frames more.
                const bool cued = frame.wait == WaitActionable ? actionable_ : frame.wait == WaitHit ? hit_ : true;
                // A free frame is pressed on as predicted, plus the offset; seen,
                // it releases the press the frame after, so a prediction is never late.
                const bool predicted = frame.wait == WaitActionable && untilActionable_ >= 0 && untilActionable_ <= 1 - frame.offset;
                const int after = frame.wait == WaitActionable ? (std::max)(0, frame.offset - 1) : (std::max)(0, static_cast<int>(frame.offset));
                const bool met = (cued && (cued ? waitedPast_++ : 0) >= after) || predicted;
                // The wait gives up only on the cue; the offset's frames after it are always granted.
                if (!met && ++waited_ < (frame.wait == WaitHit ? MaxWaitHitFrames : MaxWaitFrames) + (std::max)(0, static_cast<int>(frame.offset))) return;
            }
            // A hit is kept until the next press starts a move of its own, so
            // one landing while a cancel's motion is still going is not lost.
            // Limitation: a hit during a held button is dropped; track edges per button if it matters.
            waited_ = waitedPast_ = 0;
            if ((frame.raw & AttackButtons) && !(lastRaw_ & AttackButtons)) hit_ = false;
            lastRaw_ = frame.raw;
            if (++view_.cursor == view_.lengths[view_.selected]) {
                if (view_.loop && resume_ < 0) view_.cursor = 0;
                else Stop();
            }
        }
    }
private:
    View view_;
    std::array<std::vector<Input>, SlotCount + 1> slots_;
    bool actionable_ = false, hit_ = false, replyDue_ = false;
    int untilActionable_ = -1;
    int waited_ = 0, waitedPast_ = 0;
    unsigned lastRaw_ = 0;
    // The selection a reply borrowed, -1 when none plays.
    int resume_ = -1, resumeSide_ = 1;
    ReplySchedule schedule_;
    std::uint64_t replies_ = 0;
    // Player 1 stands on the left (SetPositions), as a battle starts.
    bool firstOnLeft_ = true;
    void Stop() {
        view_.mode = Mode::Idle; view_.cursor = 0; waited_ = waitedPast_ = 0; hit_ = false; lastRaw_ = 0;
        if (resume_ >= 0) { view_.selected = resume_; view_.playbackSide = resumeSide_; resume_ = -1; schedule_ = {}; }
    }
    void ClearHistory() {
        for (auto& rows : view_.history) rows.clear();
    }
    // Back to a checkpoint. What the session saw of the fight (a hit, a free
    // fighter, a wait's progress) belongs to the timeline the reset undid, and
    // so does a playback's place in its slot, whose waits that timeline cued:
    // a playback starts again from its first frame, in step with the restored
    // positions, and a reply ends, its stun gone. A recording goes on: it only
    // takes the player's input.
    void Rewind() {
        StopReply();
        if (view_.mode == Mode::Playback) view_.cursor = 0;
        actionable_ = hit_ = false; untilActionable_ = -1; waited_ = waitedPast_ = 0; lastRaw_ = 0;
        ClearHistory();
    }
};
// The frames stopped being consecutive (a gap or a reset): what the stun
// watch saw goes, and with it a reply that waits or is timed on it; a
// recording or another playback goes on.
inline void ForgetStun(DummyWatch& watch, Session& session) { watch.Reset(); session.StopReply(); }
} }
