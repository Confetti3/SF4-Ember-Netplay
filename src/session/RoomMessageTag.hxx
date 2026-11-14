#pragma once
// Classifying a room message once, where it enters a room queue. Every field
// reads the payload exactly as the reader it replaces did on its own parse:
// `type` as payload.value("type", ""), a room action as a successful
// get<RoomActionMessage>() of a message whose type is room_action.
#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "SessionTransport.hxx"
#include "SessionRecovery.hxx"
#include "RoomMessageQueue.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e { namespace session {

// A committed effect's place in the room journal, and for room state its kind,
// read from its token as it is queued. The effect gate checks the token itself
// at the queue head; one that does not read is left unmarked here and is
// dropped there.
inline void ReadCommitToken(const nlohmann::json& decoded, MessageTag& tag) {
	try {
		const auto& token = decoded.at("_commit");
		const auto term = token.at("term").get<std::uint64_t>(), revision = token.at("revision").get<std::uint64_t>();
		const auto sequence = token.at("sequence").get<std::uint64_t>(), recipient = token.at("recipient").get<std::uint64_t>();
		tag.commitTerm = term; tag.commitRevision = revision; tag.commitSequence = sequence; tag.commitRecipient = recipient;
	} catch (const std::exception&) { return; }
	if (tag.type == "data_update") tag.projection = RoomProjection::TableData;
	else if (tag.type == "room_snapshot")
		tag.projection = CarriesRoomChat(decoded) ? RoomProjection::SnapshotWithChat : RoomProjection::Snapshot;
}

inline MessageTag ClassifyRoomMessage(const nlohmann::json& decoded) {
	MessageTag tag;
	tag.classified = true;
	if (!decoded.is_object()) return tag;
	const auto type = decoded.find("type");
	if (type != decoded.end() && type->is_string()) tag.type = type->get<std::string>();
	tag.committed = decoded.contains("_commit");
	if (tag.committed) ReadCommitToken(decoded, tag);
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
