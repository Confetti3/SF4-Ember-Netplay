#pragma once
#include <cstdint>

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
namespace sf4e { namespace input {
constexpr std::uint32_t PhysicalBack = 0x100, PhysicalStart = 0x200;
// Called: the room calls the player back and the controls stay shut. GoNow:
// also, the banner offers go now on this pad.
enum class TrainingCall { None, Called, GoNow };
struct TrainingPadEvents {
    // down: Back went down with the controls shut; a save keeps where the
    // fighters stand at this moment, in case the game moves them on the press.
    bool down = false, reset = false, save = false, open = false, close = false, goNow = false;
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
            downAt_ = now; openAtPress_ = open;
            if (call == TrainingCall::GoNow && !open && !start) { events.goNow = true; used_ = true; armed_ = false; }
            else { used_ = open || start; armed_ = !start; events.down = !used_; }
        }
        if (back && start && !start_ && armed_) {
            // Under the call the chord opens nothing, and its Back still
            // neither resets nor saves.
            if (call == TrainingCall::None) (openAtPress_ ? events.close : events.open) = true;
            used_ = true; armed_ = false;
        }
        if (back && !used_ && now - downAt_ >= HoldSeconds) { events.save = true; used_ = true; }
        if (!back && back_ && !used_) events.reset = true;
        back_ = back; start_ = start;
        return events;
    }
    // The pad changed, lost focus or left Training: a press under way is
    // dropped, and a button still held counts only once it is pressed again.
    void Reset() { *this = TrainingPadGesture{}; back_ = start_ = used_ = true; }
private:
    bool back_ = false, start_ = false, armed_ = false, used_ = false, openAtPress_ = false;
    double downAt_ = 0;
};
} }
