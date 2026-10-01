#pragma once

// Where a host or join attempt failed, from the `reason` the helper puts on its
// host_unavailable, join_failed and invalid_or_incompatible_invitation errors. Only these stages are allowlisted;
// any other reason, or none, reads as Unknown and the player gets the generic
// sentence. Pure component: no game, helper or JSON dependencies, unit tested.

#include <string>

namespace sf4e {
namespace session {

enum class FailureStage {
	Unknown,
	RelayUnreachable, // this PC's home relay never came online
	HostUnreachable,  // no connection to the host opened, directly or through a relay
	ControlLost,      // connected to the host, then the link kept dropping
	InviteExpired,    // the invitation is past its hour, usually from a room since closed
	InviteOtherBuild, // the invitation comes from a different package
	InviteMalformed,  // the pasted text is cut short or is not an invitation
	InviteOwnRoom     // the invitation names this PC's own helper, copied while it still led the room
};

inline FailureStage FailureStageFromHelper(const std::string& code, const std::string& reason) {
	if (code == "invalid_or_incompatible_invitation") {
		if (reason == "expired") return FailureStage::InviteExpired;
		if (reason == "other_build" || reason == "old_version") return FailureStage::InviteOtherBuild;
		if (reason == "malformed") return FailureStage::InviteMalformed;
		if (reason == "own_room") return FailureStage::InviteOwnRoom;
		return FailureStage::Unknown;
	}
	if (code != "join_failed" && code != "host_unavailable") return FailureStage::Unknown;
	if (reason == "relay_unreachable") return FailureStage::RelayUnreachable;
	if (reason == "host_unreachable") return FailureStage::HostUnreachable;
	if (reason == "control_lost") return FailureStage::ControlLost;
	return FailureStage::Unknown;
}

// Untranslated names for logs.
inline const char* FailureStageLabel(FailureStage stage) {
	switch (stage) {
	case FailureStage::RelayUnreachable: return "relay_unreachable";
	case FailureStage::HostUnreachable: return "host_unreachable";
	case FailureStage::ControlLost: return "control_lost";
	case FailureStage::InviteExpired: return "invite_expired";
	case FailureStage::InviteOtherBuild: return "invite_other_build";
	case FailureStage::InviteMalformed: return "invite_malformed";
	case FailureStage::InviteOwnRoom: return "invite_own_room";
	default: return "unknown";
	}
}

} // namespace session
} // namespace sf4e
