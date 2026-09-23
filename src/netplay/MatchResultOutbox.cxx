#include "MatchResultOutbox.hxx"

namespace sf4e { namespace netplay {

bool MatchResultCapture::Valid() const {
    return HasRandomRoomId(roomId) && roomEpoch && generation && table < room::TableCount && slot < 2 &&
        (result == room::MatchResult::P1Win || result == room::MatchResult::P2Win || result == room::MatchResult::Draw);
}

std::string MatchResultCapture::ProfileKey() const {
    return Valid() ? ProfileResultKeyV2(roomId, table, generation) : std::string();
}

bool MatchResultOutbox::Capture(const MatchResultCapture& capture) {
    if (!capture.Valid()) return false;
    if (captured_ && capture_.roomId == capture.roomId && capture_.roomEpoch == capture.roomEpoch &&
        capture_.generation == capture.generation && capture_.table == capture.table) return false;
    capture_ = capture;
    captured_ = true;
    pending_ = true;
    persistRevision_ = 0;
    actionId_ = 0;
    retryAt_ = 0;
    preparedRevision_ = preparedTableRevision_ = 0;
    queuedRevision_ = queuedTableRevision_ = 0;
    return true;
}

void MatchResultOutbox::Reset() {
    capture_ = MatchResultCapture{};
    captured_ = false;
    pending_ = false;
    persistRevision_ = 0;
    actionId_ = 0;
    retryAt_ = 0;
    preparedRevision_ = preparedTableRevision_ = 0;
    queuedRevision_ = queuedTableRevision_ = 0;
}

MatchResultOutbox::PollResult MatchResultOutbox::Poll(const MatchResultRoomView& room,
    std::uint64_t nowMs, MatchResultSubmission& submission) {
    if (!pending_) return PollResult::None;
	// A degraded controller may expose an unavailable or preceding projection
	// while the native match still owns the captured generation. Preserve the
	// report until healthy control provides an authoritative identity fence.
	if (!room.healthy) return PollResult::None;
    if (room.roomId != capture_.roomId || !room.roomEpoch || room.roomEpoch != capture_.roomEpoch ||
        room.liveGeneration != capture_.generation || room.matchGeneration != capture_.generation) {
        pending_ = false;
        actionId_ = 0;
        retryAt_ = 0;
        return PollResult::Invalidated;
    }
    if (nowMs < retryAt_) return PollResult::None;
    submission.roomEpoch = capture_.roomEpoch;
    submission.revision = actionId_ ? queuedRevision_ : room.revision;
    submission.tableRevision = actionId_ ? queuedTableRevision_ : room.tableRevision;
    submission.generation = capture_.generation;
    submission.table = capture_.table;
    submission.result = capture_.result;
    submission.actionId = actionId_;
    if (!actionId_) {
        preparedRevision_ = submission.revision;
        preparedTableRevision_ = submission.tableRevision;
    }
    return PollResult::Ready;
}

void MatchResultOutbox::MarkQueued(std::uint64_t actionId, std::uint64_t nowMs) {
    if (!pending_ || !actionId) return;
    if (actionId_ && actionId != actionId_) return;
    if (!actionId_) {
        actionId_ = actionId;
        queuedRevision_ = preparedRevision_;
        queuedTableRevision_ = preparedTableRevision_;
    }
    retryAt_ = nowMs + RetryDelayMs;
}

void MatchResultOutbox::MarkUnsent(std::uint64_t nowMs) {
    if (pending_) retryAt_ = nowMs + UnsentRetryDelayMs;
}

bool MatchResultOutbox::ObserveReply(std::uint64_t actionId, bool accepted, bool duplicateResult) {
    if (!pending_ || !actionId_ || actionId != actionId_ || (!accepted && !duplicateResult)) return false;
    pending_ = false;
    actionId_ = 0;
    retryAt_ = 0;
    preparedRevision_ = preparedTableRevision_ = 0;
    queuedRevision_ = queuedTableRevision_ = 0;
    return true;
}

MatchResultOutbox::TerminalResult MatchResultOutbox::ObserveTerminal(const RandomRoomId& roomId,
    std::uint64_t roomEpoch, std::uint8_t table, std::uint64_t generation, room::MatchResult result) {
    if (!captured_ || roomId != capture_.roomId || roomEpoch != capture_.roomEpoch ||
        table != capture_.table || generation != capture_.generation) return TerminalResult::Unrelated;
    pending_ = false;
    actionId_ = 0;
    retryAt_ = 0;
    preparedRevision_ = preparedTableRevision_ = 0;
    queuedRevision_ = queuedTableRevision_ = 0;
    persistRevision_ = 0;
    return result == capture_.result ? TerminalResult::Confirmed : TerminalResult::Ended;
}

MatchResultOutbox::ProfilePersistence MatchResultOutbox::PersistProfile(ProfileRecord& profile,
    const ProfileStore& store) {
    if (!persistRevision_) {
        switch (PrepareProfileConsumption(profile)) {
        case ProfileConsumption::NoPersistenceRequired: return ProfilePersistence::NotRequired;
        case ProfileConsumption::Invalid: return ProfilePersistence::Released;
        case ProfileConsumption::PersistenceRequired: break;
        }
        // Every refusal (no writer, invalid or oversized snapshot, writer
        // stopping) repeats on retry, so a refused queue releases too.
        persistRevision_ = store.queue();
        if (!persistRevision_) return ProfilePersistence::Released;
    }
    if (store.saved(persistRevision_)) {
        persistRevision_ = 0;
        return ProfilePersistence::Saved;
    }
    if (store.failed()) {
        // A write that fails may keep failing (disk full, permissions). The
        // record stays in memory and the writer keeps retrying it.
        persistRevision_ = 0;
        return ProfilePersistence::Released;
    }
    return ProfilePersistence::Waiting;
}

MatchResultOutbox::ProfileConsumption MatchResultOutbox::PrepareProfileConsumption(ProfileRecord& profile) const {
    if (!captured_ || capture_.slot >= 2 ||
        (capture_.result != room::MatchResult::P1Win && capture_.result != room::MatchResult::P2Win))
        return ProfileConsumption::Invalid;
    // An unavailable record cannot be repaired by replaying the same terminal
    // receipt, and the bounded counters intentionally stop accepting results at
    // MaximumGames. Neither condition may retain the room's terminal receipt.
    const auto key = capture_.ProfileKey();
    if (!profile.available) return ProfileConsumption::NoPersistenceRequired;
    if (std::find(profile.recent.begin(), profile.recent.end(), key) != profile.recent.end())
        return ProfileConsumption::PersistenceRequired;
    if (profile.wins >= ProfileRecord::MaximumGames || profile.losses >= ProfileRecord::MaximumGames ||
        profile.losses >= ProfileRecord::MaximumGames - profile.wins)
        return ProfileConsumption::NoPersistenceRequired;
    return profile.Record(capture_.roomId, capture_.table, capture_.generation, capture_.slot, capture_.result) ?
        ProfileConsumption::PersistenceRequired : ProfileConsumption::Invalid;
}

} }
