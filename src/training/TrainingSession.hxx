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
struct Input { unsigned mapped = 0, raw = 0; unsigned char wait = 0; signed char offset = 0; };
constexpr int MinOffset = -120, MaxOffset = 120;
constexpr unsigned char WaitActionable = 1, WaitHit = 2;
// How long a waiting frame may wait before playback gives up on its
// condition: a whole recovery, or the few frames a buffered press can wait
// for its hit before the motion goes stale.
constexpr int MaxWaitFrames = 90, MaxWaitHitFrames = 15;
using Frame = std::array<Input, 2>;
struct InputRun { unsigned buttons = 0; unsigned frames = 0; };
enum class Mode { Idle, Recording, Playback };
enum class Action { Select, Record, Play, Stop, Clear, Loop, Save, Restore, ClearHistory, Load, ExportSlot, DummyState, Place, DummyPlan, Leave, LeaveNow, Stay };
// The dummy's behaviour as the game's Training menu sets it; each value is
// the menu's choice index and -1 leaves that setting as it is. action: stand,
// crouch, jump, cpu. guard: no block, after first hit, all, random.
// quickStand: quick, normal, delayed, random. counterHit: off, on, random.
// stun: normal, constant, none.
// super, revenge: the gauge settings as the menu stores them, 0 normal, 5
// max at round start, 7 infinite, 8 refill.
struct DummyState { int action = -1, guard = -1, quickStand = -1, counterHit = -1, stun = -1, super = -1, revenge = -1; };
// The logical Training-menu action and its temporary Stand override for
// reply input. Only the game thread owns this; edits during a reply change
// what is restored, leaving the native driver on Stand until playback ends.
class DummyAction {
public:
    int Read(int native) const { return saved_ >= 0 ? saved_ : native; }
    void Set(int& native, int value) { if (saved_ >= 0) saved_ = value; else native = value; }
    void BeginReply(int& native) { if (saved_ < 0) saved_ = native; native = 0; }
    void EndReply(int& native) { if (saved_ >= 0) { native = saved_; saved_ = -1; } }
private:
    int saved_ = -1;
};
constexpr int GaugeValues[] = {0, 5, 7, 8};
inline bool operator==(const DummyState& a, const DummyState& b) {
    return a.action == b.action && a.guard == b.guard && a.quickStand == b.quickStand && a.counterHit == b.counterHit && a.stun == b.stun &&
        a.super == b.super && a.revenge == b.revenge;
}
// Whether every value is -1 or a choice the menu offers.
inline bool ValidDummyState(const DummyState& s) {
    return s.action >= -1 && s.action <= 3 && s.guard >= -1 && s.guard <= 3 && s.quickStand >= -1 && s.quickStand <= 3 &&
        s.counterHit >= -1 && s.counterHit <= 2 && s.stun >= -1 && s.stun <= 2 &&
        (s.super == -1 || s.super == 0 || s.super == 5 || s.super == 7 || s.super == 8) &&
        (s.revenge == -1 || s.revenge == 0 || s.revenge == 5 || s.revenge == 7 || s.revenge == 8);
}
// What the dummy does by itself once it is free again. when: 0 nothing, 1
// after being hit, 2 after blocking, 3 on getting up, 4 any
// of them. moves: the reply as made-up input, for the dummy facing right [0]
// and left [1]; when empty it replies with the recording in slot. chance:
// percent of the times it replies. timing: frames the reply's first attack
// button lands after the dummy's first free frame. varyStance: it stands or
// crouches at random each time.
struct DummyPlan { int when = 0, slot = 0, chance = 100, timing = 0; bool varyStance = false; std::vector<Input> moves[2]; };
constexpr int MaxReplyTiming = 5;
inline bool ValidDummyPlan(const DummyPlan& plan) {
    return plan.when >= 0 && plan.when <= 4 && plan.slot >= 0 && plan.slot < SlotCount && plan.chance >= 0 && plan.chance <= 100 &&
        plan.timing >= -MaxReplyTiming && plan.timing <= MaxReplyTiming && plan.moves[0].size() <= std::size_t(MaxFrames) &&
        plan.moves[0].size() == plan.moves[1].size();
}
// A reply starts on its first pressed frame; -1 when it presses nothing.
inline int ReplyStart(const std::vector<Input>& frames) {
    const auto first = std::find_if(frames.begin(), frames.end(), [](const Input& frame) { return (frame.raw & FightButtons) != 0; });
    return first == frames.end() ? -1 : static_cast<int>(first - frames.begin());
}
// Frames a reply plays before its first attack button: its motion, which is
// done while the dummy is still held so the button meets the free frame.
inline int ReplyLead(const std::vector<Input>& frames) {
    const int start = ReplyStart(frames);
    const auto press = std::find_if(frames.begin(), frames.end(), [](const Input& frame) { return (frame.raw & AttackButtons) != 0; });
    return start < 0 || press == frames.end() ? 0 : static_cast<int>(press - frames.begin()) - start;
}
// freed: on the frame the dummy is free again, what it came out of, numbered
// as DummyPlan::when (1 hit, 2 block, 3 knockdown; a hit that ends in a
// knockdown is a knockdown). held: the same while it is still in it. until:
// frames from now to that free frame, -1 while not known. stretch: counts the
// hits, blocks and reactions, so a change says the wait began again.
struct DummySeen { int freed = 0, held = 0, until = -1; unsigned stretch = 0; };
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
        const Phase phase = dummy.valid ? ClassifyStatus(dummy.status) : Phase::Unknown;
        const int held = phase == Phase::Hit ? 1 : phase == Phase::Guard ? 2 : phase == Phase::Down ? 3 : 0;
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
        } else if (from_ && phase == Phase::Unknown) ++elapsed_;
        else if (from_) {
            if (learned_.size() >= 4096) learned_.clear();
            learned_[key_] = elapsed_ + 1; seen.freed = from_; from_ = 0;
        }
        return seen;
    }
    // The frames stopped being consecutive; what was learned stays.
    void Reset() { from_ = 0; }
private:
    using Key = std::tuple<unsigned, int, int>;
    std::map<Key, int> learned_;
    Key key_;
    FighterSample last_;
    int from_ = 0, elapsed_ = 0;
    unsigned stretch_ = 0;
};
// cause: DummySeen::freed or held. roll: any random number; it decides the chance.
inline bool DummyReplies(const DummyPlan& plan, int cause, unsigned roll) {
    return cause && (plan.when == 4 || plan.when == cause) && roll % 100 < static_cast<unsigned>(plan.chance);
}
// Whether a reply should start after this frame's observation: its first
// attack button, lead frames in, then meets the free frame (plus timing).
inline bool ReplyDue(const DummySeen& seen, int lead, int timing) {
    return seen.freed || (seen.until >= 0 && seen.until <= lead + 1 - timing);
}
// Each action reads only the fields named for it; the rest are ignored.
struct Command {
    Action action = Action::Stop; std::uint64_t generation = 0; std::uint64_t requestId = 0;
    // Select: the slot to choose.
    int slot = 0;
    // Loop: whether playback starts again at its end.
    bool loop = false;
    // Load: made-up input for the selected slot, and the side (0 or 1) that plays it back.
    std::vector<Input> frames; int side = 1;
    // Leave: a challenger is waiting, so the battle goes back to the main menu
    // after the announcer's call and a banner; the call's volume in percent, 0 for none.
    // LeaveNow: the player chose not to wait out that banner (LeaveCountdown::Hurry).
    // Stay: the call ended before the battle left, so it does not leave (LeaveCountdown::Cancel).
    int volume = 0;
    // DummyState: the settings to change.
    DummyState dummy;
    // Place: where to put Player 1 and Player 2 (x).
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
// The frames a called player's battle has left before it goes back to the
// main menu (Action::Leave). Go now (Action::LeaveNow) only shortens the same
// count, so a battle leaves by one path, once, whichever ended the wait.
class LeaveCountdown {
public:
    // False while a count is already running.
    bool Start(int frames) {
        if (left_ || frames < 1) return false;
        left_ = frames; return true;
    }
    // Go now: the next tick that may leave does. False when nothing is
    // counting (none started, or it already went) or it is already that tick.
    bool Hurry() {
        if (left_ <= 1) return false;
        left_ = 1; return true;
    }
    // The call it counted for is gone: the battle stays. False once the
    // battle has been told to leave, or when nothing is counting.
    bool Cancel() {
        if (!left_) return false;
        left_ = 0; return true;
    }
    // Once per battle update. fighting: the fight is running, the only time
    // the pause menu's exit could be used too; the last frame waits for it.
    // True on the one update the battle is to be told to leave.
    bool Tick(bool fighting) { return left_ && (left_ > 1 || fighting) && !--left_; }
    int Left() const { return left_; }
    void Reset() { left_ = 0; }
private:
    int left_ = 0;
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
        once_ = {}; actionable_ = hit_ = false; waited_ = waitedPast_ = 0; resume_ = -1;
    }
    void SetReady(bool ready) { view_.ready = ready; }
    // What the fight showed this frame, for the waiting frames of a playback:
    // whether the fighter is free now, and whether a hit landed on the other one.
    // untilActionable: frames until the fighter's script says it can act again, -1 when unknown.
    void Observe(bool actionable, bool hit, int untilActionable = -1) {
        actionable_ = actionable; hit_ = hit_ || hit; untilActionable_ = untilActionable;
    }
    void SetCheckpoint(bool saved) { view_.checkpoint = saved; }
    void SetPositions(float x0, float x1) { view_.x[0] = x0; view_.x[1] = x1; }
    // Every command passes here first, and only here is its battle checked.
    // Place, DummyPlan, Leave, LeaveNow, Stay and ExportSlot change nothing in the
    // session; accepted, the runtime carries them out.
    bool Apply(const Command& command) {
        if (!view_.available || command.generation != view_.generation) return false;
        switch (command.action) {
        case Action::Place: case Action::Leave: case Action::LeaveNow: case Action::Stay: case Action::ExportSlot: return true;
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
            view_.mode = Mode::Recording; view_.playbackSide = 1; once_[view_.selected] = false; return true;
        case Action::Load:
            if (view_.mode != Mode::Idle || command.frames.empty() || (command.side != 0 && command.side != 1)) return false;
            slots_[view_.selected].assign(command.frames.begin(), command.frames.begin() + (std::min)(command.frames.size(), std::size_t(MaxFrames)));
            view_.lengths[view_.selected] = static_cast<int>(slots_[view_.selected].size());
            view_.playbackSide = command.side; once_[view_.selected] = true; return true;
        case Action::Play:
            Stop();
            if (slots_[view_.selected].empty()) return false;
            view_.mode = Mode::Playback; return true;
        case Action::Clear:
            if (view_.mode != Mode::Idle) return false;
            slots_[view_.selected].clear(); view_.lengths[view_.selected] = 0; return true;
        case Action::Save: Stop(); return true;
        case Action::Restore:
            if (!view_.checkpoint) return false;
            Stop(); ClearHistory(); return true;
        default: return false;
        }
    }
    // The dummy's reply: the slot once on Player 2, from its first pressed
    // frame, after which the selected slot is the selected one again. Never
    // over a recording or another playback.
    bool Reply(int slot) {
        if (!view_.ready || view_.mode != Mode::Idle || slot < 0 || slot > ReplySlot) return false;
        const int first = ReplyStart(slots_[slot]);
        if (first < 0) return false;
        resume_ = view_.selected; resumeSide_ = view_.playbackSide;
        view_.selected = slot; view_.playbackSide = 1; view_.mode = Mode::Playback;
        view_.cursor = first;
        return true;
    }
    // The same with made-up input, kept in a slot of its own.
    bool Reply(std::vector<Input> frames) {
        if (!view_.ready || view_.mode != Mode::Idle || frames.size() > std::size_t(MaxFrames)) return false;
        slots_[ReplySlot] = std::move(frames); view_.lengths[ReplySlot] = static_cast<int>(slots_[ReplySlot].size());
        return Reply(ReplySlot);
    }
    bool Replying() const { return resume_ >= 0; }
    // The dummy was hit again: the reply it had begun is dropped.
    void StopReply() { if (resume_ >= 0) Stop(); }
    Frame Prepare(Frame physical) const {
        if (!view_.available || !view_.ready) return physical;
        if (view_.mode == Mode::Recording) {
            physical[1] = physical[0]; physical[0] = Input{};
        } else if (view_.mode == Mode::Playback) {
            physical[view_.playbackSide] = slots_[view_.selected][view_.cursor];
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
            slot.push_back(output[1]);
            view_.lengths[view_.selected] = static_cast<int>(slot.size());
            view_.cursor = static_cast<int>(slot.size());
            if (slot.size() == MaxFrames) Stop();
        } else if (view_.mode == Mode::Playback) {
            // A waiting frame repeats until its condition shows, or long enough.
            const auto& frame = slots_[view_.selected][view_.cursor];
            // The cue, then the offset's frames more.
            const bool cued = frame.wait == WaitActionable ? actionable_ : frame.wait == WaitHit ? hit_ : true;
            // A free frame is pressed on as predicted, plus the offset; seen,
            // it releases the press the frame after, so a prediction is never late.
            const bool predicted = frame.wait == WaitActionable && untilActionable_ >= 0 && untilActionable_ <= 1 - frame.offset;
            const int after = frame.wait == WaitActionable ? (std::max)(0, frame.offset - 1) : (std::max)(0, static_cast<int>(frame.offset));
            const bool met = (cued && (cued ? waitedPast_++ : 0) >= after) || predicted;
            // The wait gives up only on the cue; the offset's frames after it are always granted.
            if (!met && ++waited_ < (frame.wait == WaitHit ? MaxWaitHitFrames : MaxWaitFrames) + (std::max)(0, static_cast<int>(frame.offset))) return;
            // A hit is kept until the next press starts a move of its own, so
            // one landing while a cancel's motion is still going is not lost.
            // Limitation: a hit during a held button is dropped; track edges per button if it matters.
            waited_ = waitedPast_ = 0;
            if ((frame.raw & AttackButtons) && !(lastRaw_ & AttackButtons)) hit_ = false;
            lastRaw_ = frame.raw;
            if (++view_.cursor == view_.lengths[view_.selected]) {
                if (view_.loop && !once_[view_.selected] && resume_ < 0) view_.cursor = 0;
                else Stop();
            }
        }
    }
private:
    View view_;
    std::array<std::vector<Input>, SlotCount + 1> slots_;
    // Loaded input plays once, whatever the loop setting.
    std::array<bool, SlotCount + 1> once_{};
    bool actionable_ = false, hit_ = false;
    int untilActionable_ = -1;
    int waited_ = 0, waitedPast_ = 0;
    unsigned lastRaw_ = 0;
    // The selection a reply borrowed, -1 when none plays.
    int resume_ = -1, resumeSide_ = 1;
    void Stop() {
        view_.mode = Mode::Idle; view_.cursor = 0; waited_ = waitedPast_ = 0; hit_ = false; lastRaw_ = 0;
        if (resume_ >= 0) { view_.selected = resume_; view_.playbackSide = resumeSide_; resume_ = -1; }
    }
    void ClearHistory() {
        for (auto& rows : view_.history) rows.clear();
    }
};
} }
