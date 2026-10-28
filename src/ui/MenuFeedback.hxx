#pragma once
#include "MenuNavigation.hxx"
#include <cmath>
#include <cstdint>
#include <set>

namespace sf4e { namespace ui {
inline double RebaseUiTimestamp(double timestamp, double previousNow, double now) {
    return now + (timestamp - previousNow);
}

// ImGui time restarts when the DX9 reset path recreates its context. Keep UI
// deadlines monotonic without changing game, transport or simulation clocks.
class UiClock {
public:
    double Update(double raw) {
        if (!std::isfinite(raw) || raw < 0) return time_;
        if (!initialized_) { time_ = raw; initialized_ = true; }
        else time_ += raw >= previous_ ? raw - previous_ : raw;
        previous_ = raw;
        return time_;
    }
private:
    bool initialized_ = false;
    double previous_ = 0, time_ = 0;
};

// Smooth only a caller-marked transient checkpoint. This object supplies colors
// and explanatory hints, never the entries used to dispatch an action.
// A row starts over when its label, value or kind changes. They are compared
// field by field with the row's state, so a frame whose rows did not change
// builds no strings.
class MenuVisualFeedback {
public:
    void Update(const std::string& screen, const std::vector<MenuEntry>& entries, double now) {
        if (screen != screen_ || now < previousTime_) states_.clear();
        screen_ = screen; previousTime_ = now;
        // A row this pass does not mark is dropped below.
        ++pass_;
        for (const auto& entry : entries) {
            auto found = states_.find(entry.id);
            const unsigned flags = (entry.adjustable ? 1u : 0u) | (entry.text ? 2u : 0u) | (entry.confirm ? 4u : 0u);
            if (found == states_.end() || found->second.label != entry.label || found->second.value != entry.value ||
                found->second.flags != flags) {
                auto& state = found == states_.end() ? states_[entry.id] : found->second;
                state.label = entry.label; state.value = entry.value; state.flags = flags;
                state.visible = state.candidate = entry.enabled; state.since = now;
                state.transientUntil = entry.pending ? now + .5 : 0;
                state.pendingCandidate = state.pendingVisible = entry.pending; state.pendingSince = now;
                state.pass = pass_;
                continue;
            }
            auto& state = found->second;
            state.pass = pass_;
            if (entry.pending) state.transientUntil = now + .5;
            // The checkpoint flag itself churns per revision. The "Updating
            // room..." hint follows it only once it has held either way.
            if (state.pendingCandidate != entry.pending) { state.pendingCandidate = entry.pending; state.pendingSince = now; }
            if (now - state.pendingSince >= .25) state.pendingVisible = state.pendingCandidate;
            const bool softRecovery = entry.enabled && now < state.transientUntil;
            if (!entry.pending && !softRecovery) {
                state.visible = state.candidate = entry.enabled;
                state.since = now;
            } else {
                if (state.candidate != entry.enabled) {
                    state.candidate = entry.enabled; state.since = now;
                }
                if (now - state.since >= .25) state.visible = state.candidate;
            }
        }
        for (auto it = states_.begin(); it != states_.end();) {
            if (it->second.pass != pass_) it = states_.erase(it); else ++it;
        }
    }
    bool Enabled(const MenuEntry& entry) const {
        const auto found = states_.find(entry.id);
        return found == states_.end() ? entry.enabled : found->second.visible;
    }
    bool Pending(const MenuEntry& entry) const {
        const auto found = states_.find(entry.id);
        return (found == states_.end() ? entry.pending : found->second.pendingVisible) ||
            (entry.enabled && found != states_.end() && !found->second.visible && previousTime_ < found->second.transientUntil);
    }
private:
    struct State {
        std::string label, value; unsigned flags = 0;
        bool visible = false, candidate = false; double since = 0, transientUntil = 0;
        bool pendingCandidate = false, pendingVisible = false; double pendingSince = 0;
        // The Update that last saw the row.
        std::uint64_t pass = 0;
    };
    std::map<std::string, State> states_;
    std::string screen_;
    double previousTime_ = 0;
    std::uint64_t pass_ = 0;
};
} }
