#pragma once
#include "SessionTransport.hxx"
#include <cstdint>
#include <deque>
#include <algorithm>
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

} }
