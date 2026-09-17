#pragma once
#include "MenuNavigation.hxx"
#include <cmath>
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
class MenuVisualFeedback {
public:
    void Update(const std::string& screen, const std::vector<MenuEntry>& entries, double now) {
        if (screen != screen_ || now < previousTime_) states_.clear();
        screen_ = screen; previousTime_ = now;
        std::set<std::string> live;
        for (const auto& entry : entries) {
            live.insert(entry.id);
            auto found = states_.find(entry.id);
            const std::string signature = entry.label + '\n' + entry.value + '\n' +
                (entry.adjustable ? "a" : "-") + (entry.text ? "t" : "-") + (entry.confirm ? "c" : "-");
            if (found == states_.end() || found->second.signature != signature) {
                states_[entry.id] = {signature, entry.enabled, entry.enabled, now, entry.pending ? now + .5 : 0, entry.pending, entry.pending, now};
                continue;
            }
            auto& state = found->second;
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
            if (!live.count(it->first)) it = states_.erase(it); else ++it;
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
        std::string signature; bool visible, candidate; double since, transientUntil;
        bool pendingCandidate = false, pendingVisible = false; double pendingSince = 0;
    };
    std::map<std::string, State> states_;
    std::string screen_;
    double previousTime_ = 0;
};
} }
