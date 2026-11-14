#pragma once
#include "SessionTransport.hxx"
#include <cstdint>
#include <deque>
#include <algorithm>
#include <iterator>
#include <nlohmann/json.hpp>

namespace sf4e { namespace session {
// What a room queue refused a message for: its byte or message bound, or, in
// the client queue, a commit too far past the oldest one it still waits on.
enum class RoomQueueResult { Queued, BytesFull, MessagesFull, CommitsBehind };
constexpr std::size_t MaximumRoomQueueMessages = 64;
constexpr std::size_t MaximumRoomQueueBytes = 4 * 1024 * 1024;
// Verification frames are best effort: either queue keeps the newest of them,
// in at most half its slots.
constexpr std::size_t MaximumQueuedVerification = MaximumRoomQueueMessages / 2;
// The client queue holds the room's live effects until the checkpoint of their
// commit applies here. The helper sends this PC the newest commit each time,
// so one checkpoint catches up on any number of them; a queue that still waits
// on its oldest effect this many commits later has stopped applying commits.
constexpr std::uint64_t MaximumParkedCommits = 128;

inline bool IsVerificationType(const std::string& type) {
    return type == "battle_hash" || type == "battle_snapshot";
}
inline bool IsRoomVerificationMessage(const Message& message) {
    // Classified where it was queued (RoomMessageTag.hxx); other transports'
    // messages are parsed here.
    if (message.tag.classified) return message.tag.verification;
    try {
        const auto payload = nlohmann::json::parse(message.payload);
        return IsVerificationType(payload.value("type", std::string()));
    } catch (const std::exception&) { return false; }
}

// Either queue. Verification is best effort: keep the newest bounded window
// while quorum commits pause the server or a checkpoint parks the client,
// reserving the rest of the queue for room messages, which are never
// discarded here.
inline RoomQueueResult QueueRoomMessage(std::deque<Message>& destination,
    std::size_t& queuedBytes, Message message) {
    const bool diagnostic = IsRoomVerificationMessage(message);
    auto diagnostics = static_cast<std::size_t>(std::count_if(destination.begin(), destination.end(), IsRoomVerificationMessage));
    while ((diagnostic && diagnostics >= MaximumQueuedVerification) ||
        destination.size() >= MaximumRoomQueueMessages ||
        message.payload.size() > MaximumRoomQueueBytes - queuedBytes) {
        const auto oldest = std::find_if(destination.begin(), destination.end(), IsRoomVerificationMessage);
        if (oldest == destination.end()) {
            if (diagnostic) return RoomQueueResult::Queued;
            break;
        }
        queuedBytes -= oldest->payload.size();
        destination.erase(oldest);
        --diagnostics;
    }
    if (message.payload.size() > MaximumRoomQueueBytes - queuedBytes) return RoomQueueResult::BytesFull;
    if (destination.size() >= MaximumRoomQueueMessages) return RoomQueueResult::MessagesFull;
    queuedBytes += message.payload.size();
    destination.push_back(std::move(message));
    return RoomQueueResult::Queued;
}

// Where the client queue's first effect of a commit not applied yet is (its
// size when there is none): everything before it can be delivered now.
inline std::size_t FirstUnappliedEffect(const std::deque<Message>& queue, std::uint64_t appliedRevision) {
    const auto found = std::find_if(queue.begin(), queue.end(),
        [&](const Message& queued) { return queued.tag.commitRevision > appliedRevision; });
    return static_cast<std::size_t>(std::distance(queue.begin(), found));
}

// A committed room_snapshot or data_update is state that the next one of its
// kind for the same member replaces. While a checkpoint has not applied here,
// a busy room queues one of each per commit, so `incoming` drops what it
// replaces from its sender's run of state at the end of the queue. It keeps
// the first of its kind in the run, which the next checkpoint may deliver and
// which marks how long the queue has waited, and a snapshot with the chat
// when `incoming` says chat_unchanged. Any other room message ends the run, so
// no state moves past an event; verification frames do not, as they keep no
// order with the room's commits. Nothing is delivered here: what stays still
// passes the effect gate at the queue head, in order.
inline void DropSupersededProjections(std::deque<Message>& queue, std::size_t& queuedBytes, const Message& incoming) {
    const auto& tag = incoming.tag;
    if (tag.projection == RoomProjection::None) return;
    const auto snapshot = [](RoomProjection kind) {
        return kind == RoomProjection::Snapshot || kind == RoomProjection::SnapshotWithChat;
    };
    auto run = queue.end();
    while (run != queue.begin()) {
        const auto& previous = *std::prev(run);
        if (previous.tag.projection == RoomProjection::None && !IsRoomVerificationMessage(previous)) break;
        --run;
    }
    bool first = true;
    for (auto queued = run; queued != queue.end();) {
        const auto& old = queued->tag;
        const bool sameStream = queued->connection == incoming.connection && old.commitRecipient == tag.commitRecipient &&
            (old.projection == tag.projection || (snapshot(old.projection) && snapshot(tag.projection)));
        const bool replaced = sameStream && !first && old.commitRevision < tag.commitRevision &&
            (old.projection != RoomProjection::SnapshotWithChat || tag.projection == RoomProjection::SnapshotWithChat);
        if (sameStream) first = false;
        if (!replaced) { ++queued; continue; }
        queuedBytes -= queued->payload.size();
        queued = queue.erase(queued);
    }
}

// The client queue: the room's live effects for this PC, waiting at the head
// for their commit's checkpoint (IrohRoom::AuthorizedEffect). A commit more
// than MaximumParkedCommits past the oldest one still waiting is refused; then
// the state it supersedes is dropped, and the shared bounds apply.
inline RoomQueueResult QueueRoomEffect(std::deque<Message>& destination, std::size_t& queuedBytes,
    Message message, std::uint64_t appliedRevision) {
    const auto oldest = FirstUnappliedEffect(destination, appliedRevision);
    const auto revision = message.tag.commitRevision;
    if (oldest < destination.size() && revision > destination[oldest].tag.commitRevision &&
        revision - destination[oldest].tag.commitRevision > MaximumParkedCommits) return RoomQueueResult::CommitsBehind;
    DropSupersededProjections(destination, queuedBytes, message);
    return QueueRoomMessage(destination, queuedBytes, std::move(message));
}
} }
