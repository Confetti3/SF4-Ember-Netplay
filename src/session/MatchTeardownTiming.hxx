#pragma once

#include <cstdint>
#include <limits>

namespace sf4e { namespace session {

// Teardown has two independent boundaries.  The native GGPO owner may keep
// the local UDP socket for an arbitrary amount of time (spectators can keep
// that session alive for up to two minutes), while the helper's close command
// has a short, bounded acknowledgement window.  Keeping this state separate
// prevents the helper deadline from aging while GGPO still owns the socket or
// while the room is only waiting for its result event.
class MatchTeardownTiming {
public:
	static constexpr std::uint64_t HelperTimeoutMs = 30000;

	void RequestEnd() { requested_ = true; }
	bool Requested() const { return requested_; }

	// Starts the helper acknowledgement deadline at the exact dispatch edge.
	// Repeated Tick calls must not extend the original deadline.
	void DispatchHelperClose(std::uint64_t now) {
		if (helperCloseDispatched_) return;
		helperCloseDispatched_ = true;
		const auto max = (std::numeric_limits<std::uint64_t>::max)();
		helperDeadline_ = now > (max - HelperTimeoutMs) ? max : now + HelperTimeoutMs;
	}

	bool HelperCloseDispatched() const { return helperCloseDispatched_; }
	std::uint64_t HelperDeadline() const { return helperDeadline_; }

	// Once every helper edge is closed, the remaining wait belongs to the room
	// result protocol.  It must not be mistaken for a stalled helper close.
	bool HelperTimedOut(std::uint64_t now, bool allHelperLinksClosed) const {
		return helperCloseDispatched_ && !allHelperLinksClosed && now >= helperDeadline_;
	}

private:
	bool requested_ = false;
	bool helperCloseDispatched_ = false;
	std::uint64_t helperDeadline_ = 0;
};

} }
