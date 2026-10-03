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
	InviteExpired,    // the invitation is past its lifetime, usually from a room since closed
	InviteOtherBuild, // the invitation comes from a different package
	InviteMalformed,  // the pasted text is cut short or is not an invitation
	InviteOwnRoom,    // the invitation names this PC's own helper, copied while it still led the room
	ShortUnavailable, // a short link was pasted and the link service did not answer
	ShortUnknown      // a short link was pasted and the link service has no room under it
};

inline FailureStage FailureStageFromHelper(const std::string& code, const std::string& reason) {
	if (code == "invalid_or_incompatible_invitation") {
		if (reason == "expired") return FailureStage::InviteExpired;
		if (reason == "other_build" || reason == "old_version") return FailureStage::InviteOtherBuild;
		if (reason == "malformed") return FailureStage::InviteMalformed;
		if (reason == "own_room") return FailureStage::InviteOwnRoom;
		if (reason == "short_unavailable") return FailureStage::ShortUnavailable;
		if (reason == "short_unknown") return FailureStage::ShortUnknown;
		return FailureStage::Unknown;
	}
	if (code != "join_failed" && code != "host_unavailable") return FailureStage::Unknown;
	if (reason == "relay_unreachable") return FailureStage::RelayUnreachable;
	if (reason == "host_unreachable") return FailureStage::HostUnreachable;
	if (reason == "control_lost") return FailureStage::ControlLost;
	return FailureStage::Unknown;
}

// The helper's own reason for a failed join or host, when it is one of the
// labels it defines; empty otherwise. For logs and fixtures: the runtime's
// wording comes from FailureStage, which leaves `timeout` and `refused` Unknown.
inline const char* FailureReasonLabel(const std::string& code, const std::string& reason) {
	if (code != "join_failed" && code != "host_unavailable") return "";
	for (const char* label : {"relay_unreachable", "host_unreachable", "timeout", "refused", "control_lost"})
		if (reason == label) return label;
	return "";
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
	case FailureStage::ShortUnavailable: return "short_unavailable";
	case FailureStage::ShortUnknown: return "short_unknown";
	default: return "unknown";
	}
}

} // namespace session
} // namespace sf4e
