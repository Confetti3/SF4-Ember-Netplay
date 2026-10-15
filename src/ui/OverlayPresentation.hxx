#pragma once
#include <cstdint>
#include <mutex>
#include "../common/BattlePause.hxx"
#include "../netplay/SessionController.hxx"
#include "../common/MenuInputCapture.hxx"

namespace sf4e { namespace ui {

inline bool PassiveOverlayShown(bool shellVisible, bool trainingControlsOpen, bool nativePaused) {
    return !shellVisible && !trainingControlsOpen && !nativePaused;
}

// Presentation state survives device recreation. Only native menu states may
// capture input; rendering never changes session or simulation state.
class OverlayPresentation {
public:
    // context: where the game is (input::MenuContext). The menu is shown at
    // the main menu, over an export and over a Practice match this PC fights.
    void Update(input::MenuContext context, netplay::MatchState match, bool offline, bool focused) {
        reopened_ = false;
        const bool atMenu = input::MenuShownIn(context);
        const bool fighting = input::FightKeepsMenuAway(netplay::Fighting(match), context);
        if (match == netplay::MatchState::PostMatch && previousMatch_ != match) reopen_ = true;
        if (offline && !previousOffline_) requested_ = false;
        if (fighting) requested_ = false;
        // The native foreground query drops for a frame or two during menu
        // transitions. Hiding the shell on each drop re-armed every input gate
        // and strobed the room controls, so availability falls only after a
        // sustained absence. A fight or Play Offline still hides immediately,
        // and so does a Practice match's end, once the fight is no longer one
        // the menu is shown over.
        awayFrames_ = atMenu ? 0 : awayFrames_ + 1;
        available_ = !fighting && (atMenu || (available_ && awayFrames_ < AwayFrames));
        focused_ = focused;
        if (available_ && reopen_) { requested_ = true; reopen_ = false; reopened_ = true; }
        previousMatch_ = match;
        previousOffline_ = offline;
    }
    bool Reopened() const { return reopened_; }
    bool Available() const { return available_; }
    // Drawn while the game has lost focus (alt-tab), so the room stays on
    // screen, and the pad still drives it there (Open, Close); only the keys
    // and the mouse need focus, and the caller gates those.
    bool Visible() const { return available_ && requested_; }
    // F10's toggle: a key, so only with the focus.
    void Toggle() { if (available_ && focused_) requested_ = !requested_; }
    void Open() { if (available_) requested_ = true; }
    void Close() { requested_ = false; }
    // Consecutive updates away from the main menu before the shell hides: ~0.5 s at 60 fps.
    static constexpr unsigned AwayFrames = 30;
private:
    bool requested_ = true, available_ = false, focused_ = true;
    unsigned awayFrames_ = 0;
    bool reopened_ = false, reopen_ = false, previousOffline_ = false;
    netplay::MatchState previousMatch_ = netplay::MatchState::None;
};

// The game's pads and Ember's menu: one owner, under one lock, for what both
// threads touch. The menu holds the pads while it is shown or an open waits
// for a frame's answer, and only where the game thread last placed it.
//
// The game thread asks for opens (the pad's Start wants the shell, the native
// Network item its Play page; opens before the next frame coalesce, and Play
// outranks Controls) and says, first in each pad update, where the game is.
// The menu may hold the pads only where the presentation is available: never
// while a match is prepared or played (but for a Practice match this PC
// fights, whose fighter gives the game neutral input while the menu is
// open), nor after it until the game is back at the main menu or over an
// export, and elsewhere only for AwayFrames,
// which ride out the native menu's brief drops. So what the drawing thread
// last published never holds the pads once the menu has gone, even with no
// frame drawn to say it hid.
//
// The drawing thread takes the opens, then publishes the menu shown or not
// together with the opens it answered, in one step. An open therefore holds
// the pads from the moment it is asked until the frame that took it has
// shown the menu, with no moment between where a reader sees neither.
class MenuPads {
public:
    enum class Kind : int { None = 0, Controls = 1, Play = 2 };
    struct Taken { Kind kind = Kind::None; std::uint32_t through = 0; };
    // Game thread.
    void Request(Kind kind) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (kind > kind_) kind_ = kind;
        ++posted_;
    }
    void Place(input::MenuContext context, bool fighting) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool away = input::FightKeepsMenuAway(fighting, context);
        if (away) away_ = OverlayPresentation::AwayFrames;
        else if (input::MenuShownIn(context)) away_ = 0;
        else if (away_ < OverlayPresentation::AwayFrames) ++away_;
        startOpens_ = !away && input::ControllerOpensMenu(context);
    }
    // Start opens the menu where the game was last placed.
    bool StartOpens() const { std::lock_guard<std::mutex> lock(mutex_); return startOpens_; }
    // Drawing thread.
    Taken Take() {
        std::lock_guard<std::mutex> lock(mutex_);
        const Taken taken{kind_, posted_};
        kind_ = Kind::None;
        return taken;
    }
    void Publish(bool shown, const Taken& taken) {
        std::lock_guard<std::mutex> lock(mutex_);
        shown_ = shown; answered_ = taken.through;
    }
    // The overlay is gone (a display reset): the opens waiting are dropped and
    // nothing is shown until a frame says so again.
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        kind_ = Kind::None; answered_ = posted_; shown_ = false;
    }
    // Any thread: the menu holds the pads, and it takes them, which it also
    // does where Start opens it; they are then polled behind another window.
    bool Holds() const { std::lock_guard<std::mutex> lock(mutex_); return HoldsLocked(); }
    bool Takes() const { std::lock_guard<std::mutex> lock(mutex_); return HoldsLocked() || startOpens_; }
private:
    bool HoldsLocked() const { return away_ < OverlayPresentation::AwayFrames && (shown_ || posted_ != answered_); }
    mutable std::mutex mutex_;
    Kind kind_ = Kind::None;
    std::uint32_t posted_ = 0, answered_ = 0;
    bool shown_ = false, startOpens_ = false;
    // Updates away from where the menu can be shown. Until the first pad
    // update nothing reads the pads, and an open asked for then holds them as
    // at the main menu.
    unsigned away_ = 0;
};
} }
