#pragma once

// Decodes committed room checkpoints off the game thread.
//
// Every member of a recovery-enabled room receives every committed room
// mutation as a full checkpoint. Parsing it, verifying the effects digest and
// compacting its effect journal measured 14 to 21 ms per commit in a
// sixteen-member room, on every member, inside the 16.67 ms frame.
//
// The worker is pure by construction. It includes no room, server or transport
// header, takes only bytes and returns only values, so it cannot touch state
// that belongs to the game thread. The game thread never waits on it: Submit
// and TryTake hold the mutex only to move a queue entry. Jobs complete in
// submission order, so commits are still staged and imported in order; only
// the moment their decoded form becomes available changes.

#include "SessionRecovery.hxx"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sf4e { namespace session {

class CheckpointDecodeWorker {
public:
    struct Result {
        std::uint64_t ticket = 0;
        bool ok = false;
        SessionProposal proposal;
        // proposal.checkpoint's journal plus proposal.effects, compacted.
        std::vector<EffectEnvelope> journal;
    };

    CheckpointDecodeWorker() = default;
    CheckpointDecodeWorker(const CheckpointDecodeWorker&) = delete;
    CheckpointDecodeWorker& operator=(const CheckpointDecodeWorker&) = delete;
    ~CheckpointDecodeWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // The whole decode. Also the inline path when the worker is disabled or
    // cannot start, so both paths produce the same value.
    static Result Decode(std::uint64_t ticket, const std::string& bytes) {
        Result result;
        result.ticket = ticket;
        try {
            result.proposal = DecodeSessionProposal(nlohmann::json::parse(bytes));
            result.journal = result.proposal.checkpoint.value("effect_journal", std::vector<EffectEnvelope>{});
            result.journal.insert(result.journal.end(), result.proposal.effects.begin(), result.proposal.effects.end());
            CompactEffectJournal(result.journal);
            result.ok = true;
        } catch (const std::exception&) {
            result.ok = false;
        }
        return result;
    }

    // False when the thread could not be started; the caller decodes inline.
    bool Submit(std::uint64_t ticket, std::string bytes) {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
            jobs_.emplace_back(ticket, std::move(bytes));
        } catch (const std::exception&) {
            return false;
        }
        wake_.notify_one();
        return true;
    }

    bool TryTake(Result& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (results_.empty()) return false;
        result = std::move(results_.front());
        results_.pop_front();
        return true;
    }

private:
    void Run() {
        for (;;) {
            std::pair<std::uint64_t, std::string> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            auto result = Decode(job.first, job.second);
            std::lock_guard<std::mutex> lock(mutex_);
            results_.push_back(std::move(result));
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    // The owner bounds both queues: it never has more than its staging
    // capacity in flight, and it drains results every poll.
    std::deque<std::pair<std::uint64_t, std::string>> jobs_;
    std::deque<Result> results_;
    std::thread thread_;
    bool stop_ = false;
};

} }
