#pragma once
#include "../platform/ApplicationServices.hxx"
#include "UpdatePrompt.hxx"

namespace sf4e { namespace ui {
// Decisions for the launcher's pre-game check. Installing is always an explicit
// answer. A failed/offline check continues to SF4, while a failed install stays
// in the Updates window so its error is visible.
class StartupUpdateFlow {
public:
    enum class Action { None, Ask, Continue, Install, Skip };
    explicit StartupUpdateFlow(bool enabled) : state_(enabled ? State::Checking : State::Manual) {}
    Action Observe(const platform::ServiceSnapshot& snapshot) {
        if (snapshot.pending) return Action::None;
        if (state_ == State::Skipping) {
            state_ = State::Manual;
            return snapshot.lastAction == platform::ServiceAction::SkipUpdate && snapshot.succeeded
                ? Action::Continue : Action::None;
        }
        if (state_ != State::Checking || snapshot.launchCheck != platform::LaunchCheck::Settled) return Action::None;
        offer_ = snapshot.launchOffer;
        state_ = offer_ ? State::Asking : State::Manual;
        return offer_ ? Action::Ask : Action::Continue;
    }
    Action Answer(NoticeResult answer) {
        if (state_ != State::Asking) return Action::None;
        state_ = State::Manual;
        if (answer == NoticeResult::UpdateNow) return Action::Install;
        if (answer == NoticeResult::SkipUpdate) { state_ = State::Skipping; return Action::Skip; }
        return Action::Continue;
    }
    void Manual() { state_ = State::Manual; }
    bool Asking() const { return state_ == State::Asking; }
    bool Checking() const { return state_ == State::Checking; }
    const std::optional<launcher::InstallOffer>& Offer() const { return offer_; }
private:
    enum class State { Checking, Asking, Skipping, Manual };
    State state_;
    std::optional<launcher::InstallOffer> offer_;
};
} }
