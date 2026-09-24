#pragma once

#include "SessionController.hxx"

#include <cstdint>
#include <memory>
#include <utility>

namespace sf4e { namespace netplay {

// What one dispatch attempt did with a command.
enum class DispatchOutcome {
    Deferred,   // parked again: the room cannot take it yet
    Dispatched, // executed or sent
    Dropped,    // no longer applicable, or refused and reported
};

// A player press the room could not take yet. It is held until it can be
// dispatched, until its budget runs out, or until its generation ends.
//
// The budget belongs to the intent, not to each attempt: a retry that has to
// wait again keeps the time it started with (ledger H-006). A deferral without
// a budget waits on something that ends by itself, such as the match teardown
// a room action triggered, and never expires.
template <typename Command> class ParkedIntent {
public:
    // When the intent is complete. A room action or lobby edit is done once
    // dispatched. A Ready press stays armed after it is sent until its seat
    // flag commits (Commit) or it fails (Clear).
    enum class Completion { OnDispatch, OnCommit };
    enum class Budget { Timed, Untimed };

    ParkedIntent(std::uint64_t budgetMs, Completion completion) : budgetMs_(budgetMs), completion_(completion) {}

    // Starts the budget unless one is already running.
    void Arm(std::uint64_t nowMs, const Generation& generation) {
        if (deadlineMs_) return;
        deadlineMs_ = nowMs + budgetMs_;
        generation_ = generation;
    }
    // Holds the command for a later attempt, keeping a running budget.
    void Defer(Command command, const Generation& generation, std::uint64_t nowMs, Budget budget) {
        command_.reset(new Command(std::move(command)));
        generation_ = generation;
        if (budget == Budget::Timed) Arm(nowMs, generation);
    }
    // The parked command for the next attempt. Hand the attempt's outcome
    // back to Settle.
    std::unique_ptr<Command> Take() { return std::move(command_); }
    void Settle(DispatchOutcome outcome) {
        if (outcome != DispatchOutcome::Deferred && completion_ == Completion::OnDispatch) Clear();
    }
    // Drops the parked command without ending the intent: a running budget
    // still reports the press if nothing commits it.
    void Withdraw() { command_.reset(); }
    // A sent Ready's seat flag committed, or the table already moved on.
    void Commit() {
        if (AwaitingCommit()) Clear();
    }
    // A newer press supersedes this intent, or it failed or was cancelled.
    void Clear() {
        command_.reset();
        deadlineMs_ = 0;
    }

    const Command* Parked() const { return command_.get(); }
    // Sent and waiting for its commit, with nothing left to retry.
    bool AwaitingCommit() const { return deadlineMs_ && !command_; }
    bool Armed() const { return deadlineMs_ != 0; }
    bool Active() const { return command_ || deadlineMs_; }

    // An intent from an older room or match generation ends silently.
    void DropStale(const Generation& current) {
        if (Active() && !(generation_ == current)) Clear();
    }
    // True once an armed budget has run out; the caller reports it and clears.
    // A paused intent cannot be attempted at all, so its budget restarts in
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
    Completion completion_;
};

} }
