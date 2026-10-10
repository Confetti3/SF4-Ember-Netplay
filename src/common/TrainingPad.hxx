#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include "TrainingCallInput.hxx"

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
    // Back went down this sample, whatever the press turns out to be: the
    // moment the press takes its owner (TrainingPadInput).
    bool pressed = false;
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
            downAt_ = now; openAtPress_ = open; mine_ = !start; events.pressed = true;
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
        // Held half a second is a save, whether a sample sees Back still down
        // then or the release is the first sample past it.
        const bool held = now - downAt_ >= HoldSeconds;
        if (back && !used_ && held) { events.save = true; used_ = true; }
        if (!back && back_ && !used_) (held ? events.save : events.reset) = true;
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

// The position the gesture asked for, one at a time and in order, for the drawing
// thread: each with the Training battle (its generation) and the pad owner
// (epoch) it was pressed under. A save keeps where the fighters stood as its
// Back went down.
struct TrainingPadEvent {
    enum class Kind : std::uint8_t { Reset, Save };
    Kind kind = Kind::Reset;
    std::uint64_t generation = 0;
    std::uint32_t epoch = 0;
    float place[2] = {0, 0};
};
// The training controls' one controller, shared by the game thread, the
// drawing thread and the window procedure. It holds, under one lock:
// - whether the window has the focus, and the focus period and pad owner as
//   one epoch. LoseFocus and GainFocus each end the epoch, and Invalidate
//   ends it on a change of pad or context. Anything that would open the
//   controls first samples a Token, before it reads any input or decides
//   anything, and opens them only while that token's epoch is still the
//   current one and the window has the focus: a pad poll or a frame that
//   began before the focus went, or in the focus period before this one,
//   can never open them;
// - whether the controls are meant to be open. Closing them needs no token.
//   The pad's chord decides open or close against them, accepted only under
//   the epoch it was pressed under, so two chords before a frame open and
//   then close. Whether Ember takes the game's input follows them directly
//   (sf4e__Overlay.cxx: CapturesMenuInput), with nothing latched beside it;
// - the position events the pad posts, in order, for the drawing thread.
//   Bounded: past the bound an event is dropped, never merged into another.
//   An event posted or applied under an owner that has gone is dropped.
class TrainingControls {
public:
    static constexpr std::size_t MostEvents = 16;
    // The focus period and pad owner an input was sampled under.
    struct Token { std::uint32_t epoch = 0; bool focused = false; };
    Token Sample() const { std::lock_guard<std::mutex> lock(mutex_); return Token{epoch_, focused_}; }
    bool Open() const { std::lock_guard<std::mutex> lock(mutex_); return open_; }
    // F6, the HUD's chip, F7's recordings: open under token, or nothing.
    bool Open(const Token& token) { std::lock_guard<std::mutex> lock(mutex_); return OpenUnder(token.epoch, token.focused); }
    // F6: closed if open, otherwise opened under token.
    bool Toggle(const Token& token) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) { open_ = false; return true; }
        return OpenUnder(token.epoch, token.focused);
    }
    // The controls' own Back, the call, leaving Training.
    void Close() { std::lock_guard<std::mutex> lock(mutex_); open_ = false; }
    // View's go now on the call, pressed under epoch: put to the call's gate
    // only while that epoch is still current and the window has the focus,
    // decided in the same step as the press, so a View sampled in a focus
    // period that has ended never reaches the call.
    bool PressGoNow(std::uint32_t epoch, GoNowGate& gate, GoNowGate::Source source, bool free) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!focused_ || epoch != epoch_) return false;
        return gate.Press(source, free);
    }
    // The pad's chord, pressed under epoch: false, changing nothing, when that
    // owner has gone, or when it would open them without the focus.
    bool Accept(bool open, std::uint32_t epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open) { if (epoch != epoch_) return false; open_ = false; return true; }
        return OpenUnder(epoch, true);
    }
    std::uint32_t Epoch() const { std::lock_guard<std::mutex> lock(mutex_); return epoch_; }
    bool Focused() const { std::lock_guard<std::mutex> lock(mutex_); return focused_; }
    void Invalidate() { std::lock_guard<std::mutex> lock(mutex_); ++epoch_; events_.clear(); }
    // The window procedure's, on WM_ACTIVATEAPP. Losing the focus closes the
    // controls in the same step that ends the epoch.
    void LoseFocus() { std::lock_guard<std::mutex> lock(mutex_); ++epoch_; events_.clear(); open_ = false; focused_ = false; }
    void GainFocus() { std::lock_guard<std::mutex> lock(mutex_); ++epoch_; events_.clear(); focused_ = true; }
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
    // Whether a taken event's owner is still the pad's, checked as each is
    // applied: one taken just before the owner changed is not applied.
    bool Current(const TrainingPadEvent& event) const { return event.epoch == Epoch(); }
private:
    bool OpenUnder(std::uint32_t epoch, bool focused) {
        if (!focused || !focused_ || epoch != epoch_) return false;
        open_ = true;
        return true;
    }
    mutable std::mutex mutex_;
    bool open_ = false;
    // The window starts with the focus, as the overlay assumes.
    bool focused_ = true;
    std::uint32_t epoch_ = 0;
    std::vector<TrainingPadEvent> events_;
};

// Whether Ember takes the game's input: while its menu is shown with the
// focus (shellShown, the drawing thread's) or the training controls are
// open, as their controller says now. Nothing about the controls is latched
// beside it, so a chord that opens them between a frame's read and its
// publication is never undone by that frame.
inline bool CapturesInput(bool shellShown, const TrainingControls& controls) { return shellShown || controls.Open(); }

// Who a Back press belongs to: the Training battle (its generation) and the
// pad owner (the epoch of the TrainingControls token its poll took first) it
// went down under.
struct PadOwner {
    std::uint64_t generation = 0;
    std::uint32_t epoch = 0;
    bool operator==(const PadOwner& other) const { return generation == other.generation && epoch == other.epoch; }
    bool operator!=(const PadOwner& other) const { return !(*this == other); }
};
// The gesture with its owner, on the game thread. Each Back press takes the
// owner current as it goes down and keeps it to the end: when the battle or
// the pad owner changes before the gesture ends (focus lost and back between
// two polls, another battle), the gesture is dropped, so nothing it would ask
// for, a reset, a save or the controls, acts under another owner. The chord
// opens or closes the controls, accepted only for the owner it was pressed
// under: one whose owner went during the update (focus lost) is refused.
class TrainingPadInput {
public:
    struct Result {
        TrainingPadEvents events;
        // The owner and the fighters' places the press went down with.
        PadOwner owner;
        float place[2] = {0, 0};
    };
    Result Update(std::uint32_t physical, TrainingControls& controls, double now, TrainingCall call, const PadOwner& current, const float (&place)[2]) {
        if (held_ && current != owner_) gesture_.Reset();
        Result result;
        result.events = gesture_.Update(physical, controls.Open(), now, call);
        if (result.events.pressed) { owner_ = current; place_[0] = place[0]; place_[1] = place[1]; }
        held_ = (physical & PhysicalBack) != 0;
        if ((result.events.open || result.events.close) && !controls.Accept(result.events.open, owner_.epoch))
            result.events.open = result.events.close = false;
        result.owner = owner_; result.place[0] = place_[0]; result.place[1] = place_[1];
        return result;
    }
    // The pad changed, lost focus or left Training (TrainingPadGesture::Reset).
    void Reset() { gesture_.Reset(); held_ = false; }
private:
    TrainingPadGesture gesture_;
    PadOwner owner_;
    float place_[2] = {0, 0};
    bool held_ = false;
};
} }
