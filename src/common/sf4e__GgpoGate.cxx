#include "sf4e__GgpoGate.hxx"

namespace sf4e {
namespace gate {

const char* PhaseName(int phase) {
	switch (phase) {
	case PHASE_NO_SESSION:          return "no_session";
	case PHASE_WAITING_FOR_RUNNING: return "waiting_for_running";
	case PHASE_RUNNING:             return "running";
	case PHASE_BATTLE_CLOSING:      return "battle_closing";
	default:                        return "?";
	}
}

// Error codes from the pinned fork's ggponet.h (adanducci/ggpo@c88b667):
//   0 SUCCESS, -1 GENERAL_FAILURE, 1 INVALID_SESSION, 2 INVALID_PLAYER_HANDLE,
//   3 PLAYER_OUT_OF_RANGE, 4 PREDICTION_THRESHOLD, 5 UNSUPPORTED,
//   6 NOT_SYNCHRONIZED, 7 IN_ROLLBACK, 8 INPUT_DROPPED,
//   9 PLAYER_DISCONNECTED, 10 TOO_MANY_SPECTATORS, 11 INVALID_REQUEST
AdvancePolicy ClassifyGgpoResult(int code) {
	switch (code) {
	case 0:  // GGPO_OK
		return POLICY_CONTINUE;
	case 4:  // PREDICTION_THRESHOLD
		return POLICY_STALL_PREDICTION;
	case 6:  // NOT_SYNCHRONIZED — startup or resync; stay gated, don't sim
		return POLICY_SKIP_NOT_SYNCED;
	case 7:  // IN_ROLLBACK — should not reach our call sites; skip defensively
	case 8:  // INPUT_DROPPED — input queue overflow for one frame; skip
		return POLICY_SKIP_OTHER;
	default:
		// GENERAL_FAILURE, INVALID_SESSION, INVALID_PLAYER_HANDLE,
		// PLAYER_OUT_OF_RANGE, UNSUPPORTED, PLAYER_DISCONNECTED,
		// TOO_MANY_SPECTATORS, INVALID_REQUEST, and anything unknown:
		// irrecoverable at these call sites; log and abort orderly.
		return POLICY_FATAL;
	}
}

} // namespace gate
} // namespace sf4e
