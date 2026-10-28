#pragma once
// Classifying a room message once, where it enters a room queue. Every field
// reads the payload exactly as the reader it replaces did on its own parse:
// `type` as payload.value("type", ""), a room action as a successful
// get<RoomActionMessage>() of a message whose type is room_action.
#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "SessionTransport.hxx"
#include "RoomMessageQueue.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e { namespace session {

inline MessageTag ClassifyRoomMessage(const nlohmann::json& decoded) {
	MessageTag tag;
	tag.classified = true;
	if (!decoded.is_object()) return tag;
	const auto type = decoded.find("type");
	if (type != decoded.end() && type->is_string()) tag.type = type->get<std::string>();
	tag.committed = decoded.contains("_commit");
	tag.terminalReplay = decoded.contains("_terminal_replay");
	tag.verification = IsVerificationType(tag.type);
	// Only the string "room_action" reads as MT_ROOM_ACTION.
	if (tag.type == "room_action") {
		try {
			const auto action = decoded.get<SessionProtocol::RoomActionMessage>();
			tag.roomAction = action.type == SessionProtocol::MT_ROOM_ACTION;
			tag.actionKind = static_cast<std::uint8_t>(action.action.kind);
			tag.protocolVersion = action.action.protocolVersion;
		} catch (const std::exception&) {}
	}
	return tag;
}

// For a payload not parsed yet: one parse. A payload that does not parse is
// classified as an untyped message, which is what every reader concluded.
inline MessageTag ClassifyRoomMessage(const std::string& payload) {
	const auto decoded = nlohmann::json::parse(payload, nullptr, false);
	if (decoded.is_discarded()) { MessageTag tag; tag.classified = true; return tag; }
	return ClassifyRoomMessage(decoded);
}

// The message's tag, classifying it now when its transport did not.
inline MessageTag TagOf(const Message& message) {
	return message.tag.classified ? message.tag : ClassifyRoomMessage(message.payload);
}

} }
