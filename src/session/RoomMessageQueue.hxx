#pragma once
#include "SessionTransport.hxx"
#include <deque>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace sf4e { namespace session {
enum class RoomQueueResult { Queued, BytesFull, MessagesFull };
constexpr std::size_t MaximumRoomQueueMessages = 64;
constexpr std::size_t MaximumRoomQueueBytes = 4 * 1024 * 1024;

inline bool IsVerificationType(const std::string& type) {
    return type == "battle_hash" || type == "battle_snapshot";
}
inline bool IsRoomVerificationMessage(const Message& message) {
    try {
        const auto payload = nlohmann::json::parse(message.payload);
        return IsVerificationType(payload.value("type", std::string()));
    } catch (const std::exception&) { return false; }
}

inline RoomQueueResult QueueRoomMessage(std::deque<Message>& destination,
    std::size_t& queuedBytes, Message message, bool serverQueue) {
    if (serverQueue) {
        const bool diagnostic = IsRoomVerificationMessage(message);
        // Verification is best effort. Keep the newest bounded window while
        // quorum commits pause the server, reserving half the queue for intents.
        // Never discard a committed client effect or an ordered room action.
        auto diagnostics = std::count_if(destination.begin(), destination.end(), IsRoomVerificationMessage);
        while ((diagnostic && diagnostics >= 32) ||
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
    }
    if (message.payload.size() > MaximumRoomQueueBytes - queuedBytes) return RoomQueueResult::BytesFull;
    if (destination.size() >= MaximumRoomQueueMessages) return RoomQueueResult::MessagesFull;
    queuedBytes += message.payload.size();
    destination.push_back(std::move(message));
    return RoomQueueResult::Queued;
}
} }
