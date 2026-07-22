#pragma once

// GGPO simulation gate model (Phase 2).
//
// The legacy integration folded five unrelated conditions into one boolean
// (`bUpdateAllowed`): session lifecycle, debug/manual pause, temporary
// connection warnings, prediction-limit stalls, and terminal failure. This
// model tracks each reason explicitly so overlapping transitions cannot
// clobber one another (the old "save previous boolean, restore it on resume"
// pattern was unsafe once two reasons overlapped).
//
// Pure component: no game or GGPO dependencies, unit testable. The host
// (fSystem) drives events into it and asks CanAdvanceDeterministicFrame().
//
// Deliberate semantics decided in this phase:
// - A connection warning does NOT block deterministic simulation. GGPO keeps
//   accepting local input up to the prediction window; the game stalls only
//   when GGPO itself refuses further prediction.
// - A prediction stall is per-frame state cleared when GGPO accepts progress
//   again; it never latches the gate.
// - Resume events clear only the warning; they cannot undo a manual pause or
//   a fatal transition.

#include <stdint.h>

namespace sf4e {
namespace gate {

enum SimulationPhase {
	PHASE_NO_SESSION = 0,
	PHASE_WAITING_FOR_RUNNING, // session created; GGPO not yet RUNNING
	PHASE_RUNNING,
	PHASE_BATTLE_CLOSING
};

const char* PhaseName(int phase);

// Policy for one GGPO API result at our call sites (add_local_input /
// synchronize_input / advance_frame). Codes verified against the pinned
// fork's ggponet.h (adanducci/ggpo@c88b667).
enum AdvancePolicy {
	POLICY_CONTINUE = 0,       // GGPO_OK
	POLICY_STALL_PREDICTION,   // PREDICTION_THRESHOLD: measured temporary stall
	POLICY_SKIP_NOT_SYNCED,    // NOT_SYNCHRONIZED: no sim, remain gated
	POLICY_SKIP_OTHER,         // IN_ROLLBACK/INPUT_DROPPED: skip this frame, log
	POLICY_FATAL               // invalid session/player/etc: log and abort
};

AdvancePolicy ClassifyGgpoResult(int ggpoErrorCode);

struct GgpoGateModel {
	int phase;                        // SimulationPhase
	bool manualPause;                 // debug/manual stepping
	bool fatalError;
	bool predictionStalled;           // GGPO refused local input last attempt
	bool connectionWarningActive;     // degraded quality; informational only
	uint32_t connectionWarningStartedAtMs;

	void Reset() {
		phase = PHASE_NO_SESSION;
		manualPause = false;
		fatalError = false;
		predictionStalled = false;
		connectionWarningActive = false;
		connectionWarningStartedAtMs = 0;
	}

	// --- lifecycle ---
	void OnSessionStarted() {
		// A new session clears per-session states but honors an existing
		// manual pause (the user asked for it; a rematch must not undo it).
		phase = PHASE_WAITING_FOR_RUNNING;
		fatalError = false;
		predictionStalled = false;
		connectionWarningActive = false;
		connectionWarningStartedAtMs = 0;
	}
	void OnRunning() {
		if (phase == PHASE_WAITING_FOR_RUNNING) {
			phase = PHASE_RUNNING;
		}
	}
	void OnBattleClosing() {
		if (phase != PHASE_NO_SESSION) {
			phase = PHASE_BATTLE_CLOSING;
		}
	}
	void OnSessionClosed() {
		phase = PHASE_NO_SESSION;
		predictionStalled = false;
		connectionWarningActive = false;
	}

	// --- overlapping gate reasons ---
	void SetManualPause(bool paused) { manualPause = paused; }
	void OnFatal() { fatalError = true; }

	// Returns true when the warning newly became active (alert exactly once).
	bool OnConnectionInterrupted(uint32_t nowMs) {
		if (connectionWarningActive) {
			return false;
		}
		connectionWarningActive = true;
		connectionWarningStartedAtMs = nowMs;
		return true;
	}
	// Returns true when a warning was actually active (alert exactly once).
	// Clears ONLY the warning: never manual pause, never fatal, never the
	// prediction stall (that clears when GGPO accepts progression again).
	bool OnConnectionResumed() {
		if (!connectionWarningActive) {
			return false;
		}
		connectionWarningActive = false;
		return true;
	}

	void OnPredictionThreshold() { predictionStalled = true; }
	void OnFrameAccepted() { predictionStalled = false; }

	// --- the one central question ---
	// May the next deterministic frame advance? Note the prediction stall is
	// intentionally absent: it is enforced by GGPO refusing input, and the
	// connection warning intentionally does not block.
	bool CanAdvanceDeterministicFrame() const {
		return phase == PHASE_RUNNING && !manualPause && !fatalError;
	}
};

} // namespace gate
} // namespace sf4e
