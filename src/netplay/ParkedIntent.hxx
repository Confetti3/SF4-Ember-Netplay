#pragma once

#include "SessionController.hxx"

#include <cstdint>
#include <memory>
#include <utility>

namespace sf4e { namespace netplay {

// A player press the room could not take yet. It is held until it can be
// dispatched, until its budget runs out, or until its generation ends.
//
// The budget belongs to the intent, not to each attempt: a retry that has to
// park again keeps the time it started with (ledger H-006). An intent can
// outlive its parked command (a sent Ready keeps its budget until the seat
// flag commits), and a parked command need not be armed (a room action held
// for the match teardown it triggered never expires).
template <typename Command> class ParkedIntent {
public:
    explicit ParkedIntent(std::uint64_t budgetMs) : budgetMs_(budgetMs) {}

    // A fresh press replaces whatever was held, budget included.
    void Replace(Command command, const Generation& generation) {
        Clear();
        Park(std::move(command), generation);
    }
    // Holds the command without touching a running budget.
    void Park(Command command, const Generation& generation) {
        command_.reset(new Command(std::move(command)));
        generation_ = generation;
    }
    // Starts the budget unless one is already running.
    void Arm(std::uint64_t nowMs, const Generation& generation) {
        if (deadlineMs_) return;
        deadlineMs_ = nowMs + budgetMs_;
        generation_ = generation;
    }
    // Removes the parked command for an attempt; a running budget continues.
    std::unique_ptr<Command> Take() { return std::move(command_); }
    void Clear() {
        command_.reset();
        deadlineMs_ = 0;
    }

    const Command* Parked() const { return command_.get(); }
    bool Armed() const { return deadlineMs_ != 0; }
    bool Active() const { return command_ || deadlineMs_; }

    // An intent from an older room or match generation ends silently.
    void DropStale(const Generation& current) {
        if (Active() && !(generation_ == current)) Clear();
    }
    // True once an armed budget has run out; the caller reports it and clears.
    // A paused intent cannot be submitted at all, so its budget restarts in
    // full once the pause ends.
    bool Expired(std::uint64_t nowMs, bool paused = false) {
        if (!deadlineMs_) return false;
        if (paused) {
            deadlineMs_ = nowMs + budgetMs_;
            return false;
        }
        return nowMs >= deadlineMs_;
    }

private:
    std::unique_ptr<Command> command_;
    Generation generation_;
    std::uint64_t deadlineMs_ = 0;
    std::uint64_t budgetMs_;
};

} }
