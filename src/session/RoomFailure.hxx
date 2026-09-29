#pragma once

// Where a host or join attempt failed, from the `reason` the helper puts on its
// host_unavailable and join_failed errors. Only these stages are allowlisted;
// any other reason, or none, reads as Unknown and the player gets the generic
// sentence. Pure component: no game, helper or JSON dependencies, unit tested.

#include <string>

namespace sf4e {
namespace session {

enum class FailureStage {
	Unknown,
	RelayUnreachable, // this PC's home relay never came online
	HostUnreachable,  // no connection to the host opened, directly or through a relay
	ControlLost       // connected to the host, then the link kept dropping
};

inline FailureStage FailureStageFromHelper(const std::string& code, const std::string& reason) {
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
	default: return "unknown";
	}
}

} // namespace session
} // namespace sf4e
