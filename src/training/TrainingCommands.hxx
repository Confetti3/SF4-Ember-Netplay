#pragma once
#include "TrainingSession.hxx"
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

// The commands the overlay and the window send the game thread, and the one
// order an update handles them in. The queue is the caller's to lock.
namespace sf4e { namespace training {
class CommandQueue {
public:
    static constexpr std::size_t Capacity = 32;
    // False when refused. The last place is kept for Interrupt, so a full
    // queue never refuses one. A second Interrupt for the same battle takes
    // the queued one's place at the back, so repeats never use up more than
    // that place and the latest is the one that counts; the one it replaced
    // is refused when the batch is taken.
    bool Push(const Command& command) {
        if (command.action == Action::Interrupt) {
            for (auto at = commands_.begin(); at != commands_.end(); ++at)
                if (at->action == Action::Interrupt && at->generation == command.generation) {
                    replaced_.push_back(at->requestId); commands_.erase(at); break;
                }
            if (commands_.size() >= Capacity) return false;
        } else if (commands_.size() + 1 >= Capacity) return false;
        commands_.push_back(command);
        return true;
    }
    std::size_t Size() const { return commands_.size(); }
    // Everything queued, with the request ids of the Interrupts replaced since the last take.
    void Take(std::deque<Command>& commands, std::vector<std::uint64_t>& replaced) {
        commands.clear(); commands.swap(commands_);
        replaced.clear(); replaced.swap(replaced_);
    }
    // The battle closed: what was queued for it goes unanswered, as the
    // controls forget their requests with the battle.
    void Clear() { commands_.clear(); replaced_.clear(); }
private:
    std::deque<Command> commands_;
    std::vector<std::uint64_t> replaced_;
};
// One update's commands as they are handled: dispatch in order; refused
// before dispatch, the request ids answered no.
struct CommandBatch { std::vector<Command> dispatch; std::vector<std::uint64_t> refused; };
// 1. A command for another battle is refused, an Interrupt among them.
// 2. An AdvanceRecording or Record for this battle sent before its Interrupt is
//    refused: the Interrupt came after it and wins.
// 3. The rest go in the order they were sent.
inline CommandBatch PrepareBatch(const std::deque<Command>& taken, const std::vector<std::uint64_t>& replaced, std::uint64_t generation) {
    CommandBatch batch;
    batch.refused = replaced;
    std::size_t interrupt = taken.size();
    for (std::size_t at = 0; at < taken.size(); ++at)
        if (taken[at].generation == generation && taken[at].action == Action::Interrupt) interrupt = at;
    for (std::size_t at = 0; at < taken.size(); ++at) {
        const Command& command = taken[at];
        if (command.generation != generation || (at < interrupt && interrupt < taken.size() &&
            (command.action == Action::AdvanceRecording || command.action == Action::Record)))
            batch.refused.push_back(command.requestId);
        else batch.dispatch.push_back(command);
    }
    return batch;
}
// Answers every refused command, then dispatches the rest in order and
// answers each with what dispatch returns. Acks::Note ignores a command
// without a request id.
template<class Dispatch>
void RunBatch(const CommandBatch& batch, Acks& acks, Dispatch dispatch) {
    for (const auto id : batch.refused) acks.Note(id, false);
    for (const auto& command : batch.dispatch) acks.Note(command.requestId, dispatch(command));
}
} }
