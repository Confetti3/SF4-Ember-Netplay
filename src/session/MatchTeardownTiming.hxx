#pragma once

#include <cstdint>
#include <limits>

namespace sf4e { namespace session {

// Teardown has four independent boundaries, each documented where it is
// declared.  The first two: the native GGPO owner may keep the local UDP
// socket for a while (P1 holds it for up to 10 s to drain the spectator
// streams it owns), while the helper close command has a short, bounded
// acknowledgement window.  Keeping this state separate prevents the helper
// deadline from aging while GGPO still owns the socket or while the room is
// only waiting for its result event.  The spectator exit and the room end
// follow.
class MatchTeardownTiming {
public:
	static constexpr std::uint64_t HelperTimeoutMs = 30000;
	static constexpr std::uint64_t SpectatorExitTimeoutMs = 15000;

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

	// The third boundary.  A spectator has no Ready or Rematch to retire its
	// session with, and its stream starves as soon as P1 retires, so it can
	// reach neither battle close nor its terminal acknowledgement on its own.
	// The caller owns that condition; this owns the deadline.  Arming is
	// idempotent, so repeated ticks cannot extend the original wait.
	void ArmSpectatorExit(std::uint64_t now) {
		if (spectatorExitArmed_) return;
		spectatorExitArmed_ = true;
		const auto max = (std::numeric_limits<std::uint64_t>::max)();
		spectatorExitDeadline_ = now > (max - SpectatorExitTimeoutMs) ? max : now + SpectatorExitTimeoutMs;
	}

	void ClearSpectatorExit() { spectatorExitArmed_ = false; spectatorExitDeadline_ = 0; }

	bool SpectatorExitTimedOut(std::uint64_t now) const {
		return spectatorExitArmed_ && now >= spectatorExitDeadline_;
	}

	// What the spectator does meanwhile. P1 is its only stream source: once
	// P1's link has closed and GGPO, polled after that, holds nothing left to
	// play, the view has shown everything and retires quietly. At the deadline
	// it retires anyway; only a view cut short while P1 was still streaming
	// is worth telling the player about.
	enum class SpectatorExit { Wait, Retire, RetireCutShort };
	static SpectatorExit SpectatorExitStep(bool sourceClosed, bool streamDrained, bool timedOut) {
		if (sourceClosed && streamDrained) return SpectatorExit::Retire;
		if (!timedOut) return SpectatorExit::Wait;
		return sourceClosed ? SpectatorExit::Retire : SpectatorExit::RetireCutShort;
	}

	// The fourth boundary: every helper edge is closed but the room's game_end
	// has not arrived. The room sends it on the result report or a fighter's
	// disconnect, so a long wait means the room link itself is stuck. Arming
	// is idempotent like the spectator exit.
	static constexpr std::uint64_t RoomEndTimeoutMs = 60000;

	void ArmRoomEnd(std::uint64_t now) {
		if (roomEndArmed_) return;
		roomEndArmed_ = true;
		const auto max = (std::numeric_limits<std::uint64_t>::max)();
		roomEndDeadline_ = now > (max - RoomEndTimeoutMs) ? max : now + RoomEndTimeoutMs;
	}

	bool RoomEndTimedOut(std::uint64_t now) const {
		return roomEndArmed_ && now >= roomEndDeadline_;
	}

private:
	bool requested_ = false;
	bool helperCloseDispatched_ = false;
	std::uint64_t helperDeadline_ = 0;
	bool spectatorExitArmed_ = false;
	std::uint64_t spectatorExitDeadline_ = 0;
	bool roomEndArmed_ = false;
	std::uint64_t roomEndDeadline_ = 0;
};

} }
