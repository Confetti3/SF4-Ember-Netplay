#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <vector>
#include <string>
#include "FrameMeter.hxx"
#include "TrialSession.hxx"

namespace sf4e { namespace training {
constexpr int SlotCount = 8;
constexpr int MaxFrames = 60 * 30;
constexpr int HistoryRows = 12;
constexpr unsigned FightButtons = 0xcff; // Directions and six attacks; excludes menu buttons.
constexpr unsigned AttackButtons = 0xcf0;
// wait: a loaded frame that repeats, buttons held, until the fighter can act
// again (1) or a hit lands (2), so a replayed combo takes its timing from the
// fight; offset: frames after the cue the press lands on. A free frame is
// predicted from the fighter's script, so 0 is that frame itself and a
// negative offset is before it; a hit is pressed on the frame after it is seen.
struct Input { unsigned mapped = 0, raw = 0; unsigned char wait = 0; signed char offset = 0; };
constexpr int MinOffset = -30, MaxOffset = 30;
constexpr unsigned char WaitActionable = 1, WaitHit = 2;
// How long a waiting frame may wait before playback gives up on its
// condition: a whole recovery, or the few frames a buffered press can wait
// for its hit before the motion goes stale.
constexpr int MaxWaitFrames = 90, MaxWaitHitFrames = 15;
using Frame = std::array<Input, 2>;
struct InputRun { unsigned buttons = 0; unsigned frames = 0; };
enum class Mode { Idle, Recording, Playback };
enum class Action { Select, Record, Play, Stop, Clear, Loop, Save, Restore, ClearHistory, AutoFreeze, StartTrial, StopTrial, Load, CaptureStart, CaptureStop, ExportSlot, DummyState, Place };
// The dummy's behaviour as the game's Training menu sets it; each value is
// the menu's choice index and -1 leaves that setting as it is. action: stand,
// crouch, jump, cpu. guard: no block, after first hit, all, random.
// quickStand: quick, normal, delayed, random. counterHit: off, on, random.
// stun: normal, constant, none.
// super, revenge: the gauge settings as the menu stores them, 0 normal, 5
// max at round start, 7 infinite, 8 refill.
struct DummyState { int action = -1, guard = -1, quickStand = -1, counterHit = -1, stun = -1, super = -1, revenge = -1; };
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
// trial, trialSteps: for StartTrial, the combo to practise and the text shown
// for each of its steps. The overlay prepares both; the adapter owns them after.
// trialFighter, trialTexts: the native id of the combo's fighter and, per
// step, the game's own text ids for its task list; either missing keeps the
// overlay's list.
// frames: for Load, made-up input for the selected slot; value is the side
// (0 or 1) that plays it back.
struct Command {
    Action action = Action::Stop; int value = 0; std::uint64_t generation = 0; std::uint64_t requestId = 0;
    Trial trial; std::vector<std::string> trialSteps;
    int trialFighter = -1; std::vector<std::array<std::string, 4>> trialTexts;
    // StartTrial: after every attempt, failed or cleared, the checkpoint is
    // restored so the next one starts where it was saved.
    bool trialResetOnDrop = false;
    std::vector<Input> frames;
    // DummyState: the settings to change.
    DummyState dummy;
    // Place: where to put Player 1 and Player 2 (x).
    float place[2] = {0, 0};
};
struct View {
    bool available = false, ready = false, checkpoint = false, loop = true;
    std::uint64_t generation = 0;
    // Where Player 1 and Player 2 stand (x), as the adapter reads them each frame.
    float x[2] = {0, 0};
    Mode mode = Mode::Idle;
    int selected = 0, cursor = 0, playbackSide = 1;
    std::array<int, SlotCount> lengths{};
    std::array<std::deque<InputRun>, 2> history;
    std::array<std::deque<unsigned>, 2> timeline;
    MeterView meter;
    // The running trial: one text per step, empty when none runs. The adapter
    // fills both, as it does the meter.
    std::vector<std::string> trialSteps;
    TrialView trial;
    // The game's own task list shows the trial, so the overlay's list stays hidden.
    bool nativeTrialList = false;
    // Record combo: whether Player 1's moves are being written down, and
    // the moves so far: action id, cancel when the move cancelled the one
    // before, and the frame it began on since the recording started.
    struct CapturedMove { int action = -1; bool cancel = false; int frame = 0; };
    bool capturing = false;
    std::vector<CapturedMove> captured;
    // ExportSlot: the selected slot's frames, handed over once per request.
    std::uint64_t exportId = 0; int exportedSlot = -1;
    std::vector<Input> exported;
    // One entry per move of the last replayed combo: how many frames it
    // waited for its cue (the free frame or the hit), whether the cue was
    // seen before the wait gave up, and whether a hit followed within a second.
    struct ReplayStep { int waited = 0; bool cued = true, hit = false, predicted = false; };
    std::vector<ReplayStep> replay;
    std::uint64_t commandId = 0;
    bool commandAccepted = false;
    std::string commandError;
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
        once_ = {}; actionable_ = hit_ = false; waited_ = waitedPast_ = 0;
    }
    void SetReady(bool ready) { view_.ready = ready; }
    // What the fight showed this frame, for the waiting frames of a replay:
    // whether the fighter is free now, and whether a hit landed on the other one.
    // untilActionable: frames until the fighter's script says it can act again, -1 when unknown.
    void Observe(bool actionable, bool hit, int untilActionable = -1) {
        actionable_ = actionable; hit_ = hit_ || hit; untilActionable_ = untilActionable;
        if (hit && !view_.replay.empty() && sincePress_ < 60) view_.replay.back().hit = true;
    }
    void SetCheckpoint(bool saved) { view_.checkpoint = saved; }
    void SetPositions(float x0, float x1) { view_.x[0] = x0; view_.x[1] = x1; }
    bool Apply(Command command) {
        if (!view_.available || command.generation != view_.generation) return false;
        switch (command.action) {
        case Action::Stop: Stop(); return true;
        case Action::Select:
            if (command.value < 0 || command.value >= SlotCount || view_.mode != Mode::Idle) return false;
            view_.selected = command.value; return true;
        case Action::Loop: view_.loop = command.value != 0; return true;
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
            if (view_.mode != Mode::Idle || command.frames.empty()) return false;
            slots_[view_.selected].assign(command.frames.begin(), command.frames.begin() + (std::min)(command.frames.size(), std::size_t(MaxFrames)));
            view_.lengths[view_.selected] = static_cast<int>(slots_[view_.selected].size());
            view_.playbackSide = command.value & 1; once_[view_.selected] = true; return true;
        case Action::Play:
            if (slots_[view_.selected].empty()) return false;
            Stop(); view_.mode = Mode::Playback;
            // A loaded combo reports each move; a recording has none to report.
            view_.replay.clear(); sincePress_ = 0;
            if (once_[view_.selected]) view_.replay.push_back({});
            return true;
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
            auto& timeline = view_.timeline[side];
            timeline.push_back(buttons);
            if (timeline.size() > 120) timeline.pop_front();
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
            ++sincePress_;
            if (!met && ++waited_ < (frame.wait == WaitHit ? MaxWaitHitFrames : MaxWaitFrames)) return;
            if (frame.wait) { view_.replay.push_back({waited_, met, false, predicted}); sincePress_ = 0; }
            // A hit is kept until the next press starts a move of its own, so
            // one landing while a cancel's motion is still going is not lost.
            // ponytail: a hit during a held button is dropped; track edges per button if it matters.
            waited_ = waitedPast_ = 0;
            if ((frame.raw & AttackButtons) && !(lastRaw_ & AttackButtons)) hit_ = false;
            lastRaw_ = frame.raw;
            if (++view_.cursor == view_.lengths[view_.selected]) {
                if (view_.loop && !once_[view_.selected]) view_.cursor = 0;
                else Stop();
            }
        }
    }
private:
    View view_;
    std::array<std::vector<Input>, SlotCount> slots_;
    // Loaded input is a combo: it plays once, whatever the loop setting.
    std::array<bool, SlotCount> once_{};
    bool actionable_ = false, hit_ = false;
    int untilActionable_ = -1;
    int waited_ = 0, waitedPast_ = 0, sincePress_ = 0;
    unsigned lastRaw_ = 0;
    void Stop() { view_.mode = Mode::Idle; view_.cursor = 0; waited_ = waitedPast_ = 0; hit_ = false; lastRaw_ = 0; }
    void ClearHistory() {
        for (auto& rows : view_.history) rows.clear();
        for (auto& frames : view_.timeline) frames.clear();
    }
};
} }
