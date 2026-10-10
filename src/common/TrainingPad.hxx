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
namespace sf4e { namespace input {
constexpr std::uint32_t PhysicalBack = 0x100, PhysicalStart = 0x200;
struct TrainingPadEvents {
    // down: Back went down with the controls shut; a save keeps where the
    // fighters stand at this moment, in case the game moves them on the press.
    bool down = false, reset = false, save = false, open = false, close = false;
    bool Any() const { return down || reset || save || open || close; }
};
class TrainingPadGesture {
public:
    static constexpr double HoldSeconds = .5;
    // physical: the pad's physical buttons now; open: the controls are open.
    TrainingPadEvents Update(std::uint32_t physical, bool open, double now) {
        TrainingPadEvents events;
        const bool back = (physical & PhysicalBack) != 0, start = (physical & PhysicalStart) != 0;
        if (back && !back_) {
            downAt_ = now; openAtPress_ = open; used_ = open || start; armed_ = !start;
            events.down = !used_;
        }
        if (back && start && !start_ && armed_) {
            (openAtPress_ ? events.close : events.open) = true;
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
