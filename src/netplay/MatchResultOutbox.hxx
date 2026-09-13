#pragma once

#include "ProfileRecord.hxx"

#include <cstdint>

namespace sf4e { namespace netplay {

struct MatchResultCapture {
    RandomRoomId roomId{};
    std::uint64_t roomEpoch = 0;
    std::uint64_t generation = 0;
    std::uint8_t table = 0;
    unsigned slot = 2;
    room::MatchResult result = room::MatchResult::Abort;

    bool Valid() const;
    std::string ProfileKey() const;
};

struct MatchResultRoomView {
    RandomRoomId roomId{};
    std::uint64_t roomEpoch = 0;
    std::uint64_t revision = 0;
    std::uint64_t tableRevision = 0;
    std::uint64_t matchGeneration = 0;
    std::uint64_t liveGeneration = 0;
    bool healthy = false;
};

struct MatchResultSubmission {
    std::uint64_t roomEpoch = 0;
    std::uint64_t revision = 0;
    std::uint64_t tableRevision = 0;
    std::uint64_t generation = 0;
    std::uint8_t table = 0;
    room::MatchResult result = room::MatchResult::Abort;
    // Zero allocates the first action ID. A nonzero value retries the exact
    // previously queued result identity without growing the server queue.
    std::uint64_t actionId = 0;
};

class MatchResultOutbox {
public:
    static constexpr std::uint64_t RetryDelayMs = 500;
    static constexpr std::uint64_t UnsentRetryDelayMs = 100;
    enum class PollResult { None, Ready, Invalidated };
    enum class TerminalResult { Unrelated, Ended, Confirmed };
    enum class ProfileConsumption { Invalid, PersistenceRequired, NoPersistenceRequired };

    bool Capture(const MatchResultCapture& capture);
    void Reset();
    bool Pending() const { return pending_; }
    const MatchResultCapture* Captured() const { return captured_ ? &capture_ : nullptr; }

    PollResult Poll(const MatchResultRoomView& room, std::uint64_t nowMs,
        MatchResultSubmission& submission);
    void MarkQueued(std::uint64_t actionId, std::uint64_t nowMs);
    void MarkUnsent(std::uint64_t nowMs);
    bool ObserveReply(std::uint64_t actionId, bool accepted, bool duplicateResult);
    TerminalResult ObserveTerminal(const RandomRoomId& roomId, std::uint64_t roomEpoch,
        std::uint8_t table, std::uint64_t generation, room::MatchResult result);
    ProfileConsumption PrepareProfileConsumption(ProfileRecord& profile) const;

private:
    MatchResultCapture capture_;
    bool captured_ = false;
    bool pending_ = false;
    std::uint64_t actionId_ = 0;
    std::uint64_t retryAt_ = 0;
    std::uint64_t preparedRevision_ = 0, preparedTableRevision_ = 0;
    std::uint64_t queuedRevision_ = 0, queuedTableRevision_ = 0;
};

} }
