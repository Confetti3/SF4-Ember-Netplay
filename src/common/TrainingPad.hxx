#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

// The assigned pad in offline Training, outside Ember's own menu input. It
// reads the physical Back and Start bits, which the native pad layer gives the
// same masks for an Xbox pad's View and Start and for a DirectInput pad's
// configured Select and Start (F-002 in
// docs/reports/2026-09-08_reverse-USF4-menu-input-report.md):
// - Back tapped, let go within half a second, resets the position;
// - Back held half a second saves it;
// - Start pressed while Back is held opens the training controls, or closes
//   them if they were open when Back went down. Back then neither resets nor
//   saves when it is let go; a Start later than half a second comes after
//   the save, which stands.
// Start first and Back after is the game's pause, so only a Start that goes
// down while Back is already held counts, and a Back pressed during the
// pause's Start does nothing here. With the controls open, Back alone
// does nothing here: inside them the menu's own Back button goes back.
//
// Called back from Training by a room (TRAINING_IN_ROOMS.md), the controls
// stay shut, so Back with Start opens nothing. While the call's banner offers
// go now on this pad, a fresh Back press is go now and nothing else: it
// neither resets, saves nor opens, however long it is held and whatever
// Start does after it. A Back already held when the call came is not go now;
// it ends as the tap or hold it began as.
//
// A Start that goes down while the gesture's Back is held is the gesture's
// whether or not it opens anything (TrainingPadEvents::owned), so the game
// never takes it for its pause, under the call too.
namespace sf4e { namespace input {
constexpr std::uint32_t PhysicalBack = 0x100, PhysicalStart = 0x200;
// Called: the room calls the player back and the controls stay shut. GoNow:
// also, the banner offers go now on this pad.
enum class TrainingCall { None, Called, GoNow };
struct TrainingPadEvents {
    // down: Back went down with the controls shut; a save keeps where the
    // fighters stand at this moment, in case the game moves them on the press.
    bool down = false, reset = false, save = false, open = false, close = false, goNow = false;
    // The game's menu buttons (common/MenuInputCapture.hxx: NativeStart) this
    // sample belongs to the gesture: they are cleared from the game's input
    // where it publishes it, down, held and let go.
    std::uint32_t owned = 0;
    bool Any() const { return down || reset || save || open || close || goNow; }
};
class TrainingPadGesture {
public:
    static constexpr double HoldSeconds = .5;
    // physical: the pad's physical buttons now; open: the controls are open;
    // call: whether a room is calling the player back.
    TrainingPadEvents Update(std::uint32_t physical, bool open, double now, TrainingCall call = TrainingCall::None) {
        TrainingPadEvents events;
        const bool back = (physical & PhysicalBack) != 0, start = (physical & PhysicalStart) != 0;
        if (back && !back_) {
            downAt_ = now; openAtPress_ = open; mine_ = !start;
            if (call == TrainingCall::GoNow && !open && !start) { events.goNow = true; used_ = true; armed_ = false; }
            else { used_ = open || start; armed_ = !start; events.down = !used_; }
        }
        if (!back) mine_ = false;
        // Start going down under the gesture's own Back is the gesture's,
        // until it is let go, whatever the chord does.
        if (start && !start_ && back && mine_) startOwned_ = true;
        if (back && start && !start_ && armed_) {
            // Under the call the chord opens nothing, and its Back still
            // neither resets nor saves.
            if (call == TrainingCall::None) (openAtPress_ ? events.close : events.open) = true;
            used_ = true; armed_ = false;
        }
        if (back && !used_ && now - downAt_ >= HoldSeconds) { events.save = true; used_ = true; }
        if (!back && back_ && !used_) events.reset = true;
        if (startOwned_) events.owned |= PhysicalStart;
        if (!start) startOwned_ = false;
        back_ = back; start_ = start;
        return events;
    }
    // The pad changed, lost focus or left Training: a press under way is
    // dropped, and a button still held counts only once it is pressed again.
    void Reset() { *this = TrainingPadGesture{}; back_ = start_ = used_ = true; }
private:
    bool back_ = false, start_ = false, armed_ = false, used_ = false, openAtPress_ = false;
    // mine_: the Back held now went down before Start, so it is the
    // gesture's; startOwned_: the Start held now went down under it.
    bool mine_ = false, startOwned_ = false;
    double downAt_ = 0;
};

// What the gesture asked for, one at a time and in order, for the drawing
// thread: each with the Training battle (its generation) and the pad owner
// (epoch) it was pressed under. A save keeps where the fighters stood as its
// Back went down.
struct TrainingPadEvent {
    enum class Kind : std::uint8_t { Reset, Save, Open, Close };
    Kind kind = Kind::Reset;
    std::uint64_t generation = 0;
    std::uint32_t epoch = 0;
    float place[2] = {0, 0};
};
// Between the game thread, which posts, and the drawing thread, which takes.
// Bounded: past the bound an event is dropped, never merged into another.
// Invalidate, on focus loss or a change of pad or context, drops what waits
// and every event still to be posted under the old owner.
class TrainingPadQueue {
public:
    static constexpr std::size_t MostEvents = 16;
    std::uint32_t Epoch() const { std::lock_guard<std::mutex> lock(mutex_); return epoch_; }
    void Invalidate() { std::lock_guard<std::mutex> lock(mutex_); ++epoch_; events_.clear(); }
    // False when it is dropped: posted under an owner that has gone, or full.
    bool Post(const TrainingPadEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (event.epoch != epoch_ || events_.size() >= MostEvents) return false;
        events_.push_back(event);
        return true;
    }
    std::vector<TrainingPadEvent> Take() {
        std::vector<TrainingPadEvent> taken;
        std::lock_guard<std::mutex> lock(mutex_);
        taken.swap(events_);
        return taken;
    }
private:
    mutable std::mutex mutex_;
    std::uint32_t epoch_ = 0;
    std::vector<TrainingPadEvent> events_;
};
} }
