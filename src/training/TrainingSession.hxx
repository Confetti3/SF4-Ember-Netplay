#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <vector>
#include <string>
#include "FrameMeter.hxx"

namespace sf4e { namespace training {
constexpr int SlotCount = 8;
constexpr int MaxFrames = 60 * 30;
constexpr int HistoryRows = 12;
constexpr unsigned FightButtons = 0xcff; // Directions and six attacks; excludes menu buttons.
struct Input { unsigned mapped = 0, raw = 0; };
using Frame = std::array<Input, 2>;
struct InputRun { unsigned buttons = 0; unsigned frames = 0; };
enum class Mode { Idle, Recording, Playback };
enum class Action { Select, Record, Play, Stop, Clear, Loop, Save, Restore, ClearHistory, AutoFreeze };
struct Command { Action action = Action::Stop; int value = 0; std::uint64_t generation = 0; std::uint64_t requestId = 0; };
struct View {
    bool available = false, ready = false, checkpoint = false, loop = true;
    std::uint64_t generation = 0;
    Mode mode = Mode::Idle;
    int selected = 0, cursor = 0;
    std::array<int, SlotCount> lengths{};
    std::array<std::deque<InputRun>, 2> history;
    std::array<std::deque<unsigned>, 2> timeline;
    MeterView meter;
    std::uint64_t commandId = 0;
    bool commandAccepted = false;
    std::string commandError;
};

// Owns only practice input data. The game adapter owns native save states.
// Prepare is repeatable; only Commit advances a recording or playback cursor.
class Session {
public:
    const View& GetView() const { return view_; }
    void Enter() { Reset(); view_.available = true; }
    void Reset() {
        const auto next = view_.generation + 1;
        view_ = View{}; view_.generation = next;
        for (auto& slot : slots_) slot.clear();
    }
    void SetReady(bool ready) { view_.ready = ready; }
    void SetCheckpoint(bool saved) { view_.checkpoint = saved; }
    bool Apply(Command command) {
        if (!view_.available || command.generation != view_.generation) return false;
        switch (command.action) {
        case Action::Stop: Stop(); return true;
        case Action::Select:
            if (command.value < 0 || command.value >= SlotCount || view_.mode != Mode::Idle) return false;
            view_.selected = command.value; return true;
        case Action::Loop: view_.loop = command.value != 0; return true;
        case Action::ClearHistory: ClearHistory(); return true;
        default: break;
        }
        if (!view_.ready) return false;
        switch (command.action) {
        case Action::Record:
            Stop(); slots_[view_.selected].clear(); view_.lengths[view_.selected] = 0;
            view_.mode = Mode::Recording; return true;
        case Action::Play:
            if (slots_[view_.selected].empty()) return false;
            Stop(); view_.mode = Mode::Playback; return true;
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
            physical[1] = slots_[view_.selected][view_.cursor];
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
            if (++view_.cursor == view_.lengths[view_.selected]) {
                if (view_.loop) view_.cursor = 0;
                else Stop();
            }
        }
    }
private:
    View view_;
    std::array<std::vector<Input>, SlotCount> slots_;
    void Stop() { view_.mode = Mode::Idle; view_.cursor = 0; }
    void ClearHistory() {
        for (auto& rows : view_.history) rows.clear();
        for (auto& frames : view_.timeline) frames.clear();
    }
};
} }
