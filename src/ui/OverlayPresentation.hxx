#pragma once
#include <atomic>
#include "../common/BattlePause.hxx"
#include "../netplay/SessionController.hxx"

namespace sf4e { namespace ui {

inline bool PassiveOverlayShown(bool shellVisible, bool trainingControlsOpen, bool nativePaused) {
    return !shellVisible && !trainingControlsOpen && !nativePaused;
}

// Presentation state survives device recreation. Only native menu states may
// capture input; rendering never changes session or simulation state.
class OverlayPresentation {
public:
    void Update(bool atMenu, netplay::MatchState match, bool offline, bool focused) {
        reopened_ = false;
        const bool fighting = match == netplay::MatchState::Preparing || match == netplay::MatchState::Playing;
        if (match == netplay::MatchState::PostMatch && previousMatch_ != match) reopen_ = true;
        if (offline && !previousOffline_) requested_ = false;
        if (fighting) requested_ = false;
        // The native foreground query drops for a frame or two during menu
        // transitions. Hiding the shell on each drop re-armed every input gate
        // and strobed the room controls, so availability falls only after a
        // sustained absence. A fight or Play Offline still hides immediately.
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
    // screen; only input needs focus, and the caller gates that.
    bool Visible() const { return available_ && requested_; }
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

// The game thread asks the drawing thread to open the shell: the native
// Network item wants the Play page, the pad's Start only the shell. Only the
// drawing thread touches the presentation and the shell's navigation, so a
// request waits for its next frame. Requests arriving before then coalesce,
// and Play outranks Controls.
class OpenRequests {
public:
    enum class Kind : int { None = 0, Controls = 1, Play = 2 };
    void Post(Kind kind) {
        int current = value_.load();
        while (current < static_cast<int>(kind) && !value_.compare_exchange_weak(current, static_cast<int>(kind))) {}
    }
    Kind Take() { return static_cast<Kind>(value_.exchange(static_cast<int>(Kind::None))); }
private:
    std::atomic<int> value_{0};
};
} }
