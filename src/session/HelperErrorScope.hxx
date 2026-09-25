#pragma once

// Every error code the networking helper reports, sorted by how much of the
// room it ends. IrohRoom used to decide this with a chain of string
// compares and a whitelist, and anything it did not recognise closed the
// whole room, including errors that concern one match only (F-017). The
// table is the single place that knows a code's reach; the room only acts
// on the scope.
//
// Pure component: no game, helper or JSON dependencies, unit tested.

#include <string>

namespace sf4e {
namespace session {

enum class HelperErrorScope {
	Probe,          // the connection check only
	Gameplay,       // preparing a gameplay link failed; room control stays
	Checkpoint,     // a checkpoint transfer, which the room retries
	ControlPeer,    // control to a peer other than the committed leader
	ControlLeader,  // control to the committed leader: the room degrades
	Match,          // one match; its own deadlines end it, the room stays
	RoomFatal       // the room closes
};

struct HelperErrorVerdict {
	HelperErrorScope scope;
	// Only protocol-defined labels may enter UI diagnostics. When false the
	// room reports the generic "helper_room_error" instead of the code.
	bool codeIsProtocolLabel;
};

inline const char* HelperErrorScopeName(HelperErrorScope scope) {
	switch (scope) {
	case HelperErrorScope::Probe: return "probe";
	case HelperErrorScope::Gameplay: return "gameplay";
	case HelperErrorScope::Checkpoint: return "checkpoint";
	case HelperErrorScope::ControlPeer: return "control_peer";
	case HelperErrorScope::ControlLeader: return "control_leader";
	case HelperErrorScope::Match: return "match";
	case HelperErrorScope::RoomFatal: return "room";
	}
	return "?";
}

// `coordinationActive`: the room runs committed coordination, so checkpoint
// and control errors are recoverable there. `failedPeerIsNotLeader`: the
// helper named the peer that failed and it is not the committed leader.
inline HelperErrorVerdict ClassifyHelperError(const std::string& code, bool coordinationActive,
	bool failedPeerIsNotLeader) {
	const auto startsWith = [&](const char* prefix) { return code.compare(0, std::string(prefix).size(), prefix) == 0; };
	if (code == "probe_unavailable") return { HelperErrorScope::Probe, true };
	if (code == "gameplay_prepare_failed") return { HelperErrorScope::Gameplay, true };
	if (coordinationActive && (startsWith("checkpoint_") || startsWith("invalid_checkpoint_") ||
		code == "stale_checkpoint_ack" || code == "unexpected_checkpoint_ack"))
		return { HelperErrorScope::Checkpoint, true };
	if (coordinationActive && (code == "control_send_failed" || code == "coordination_unavailable"))
		return { failedPeerIsNotLeader ? HelperErrorScope::ControlPeer : HelperErrorScope::ControlLeader, true };
	// The helper answers a command for a match it no longer holds, or a game
	// registration it cannot honour. The match's own deadlines end that
	// match; the room and the other tables are untouched.
	if (code == "stale_match" || code == "invalid_game_registration") return { HelperErrorScope::Match, true };
	const bool protocolLabel = code == "invalid_or_incompatible_invitation" || code == "join_failed" ||
		code == "host_unavailable" || code == "invalid_room_state" || code == "control_send_failed" ||
		code == "invalid_control_size";
	return { HelperErrorScope::RoomFatal, protocolLabel };
}

} // namespace session
} // namespace sf4e
