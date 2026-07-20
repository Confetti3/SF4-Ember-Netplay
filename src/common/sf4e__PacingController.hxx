#pragma once

// Time-sync pacing controller (Phase 4).
//
// GGPO's timesync event recommends slowing down by N frames when this client
// is ahead of the remote. The legacy handler slept for the full
// recommendation inside the event callback (up to ~150 ms inside DoPoll,
// blocking the outer frame). This controller instead records a bounded
// correction target; the host repays it in small slices at a controlled
// point in the outer tick, measuring the actual wait and subtracting it.
//
// Semantics verified against the pinned fork (adanducci/ggpo@c88b667,
// timesync.cpp): a recommendation is a FRESH estimate of how many frames we
// are ahead (half the averaged advantage delta, already clamped to
// MAX_FRAME_ADVANTAGE=9, gated at MIN_FRAME_ADVANTAGE=3, and emitted at most
// once per 240 frames). It is not an additive delta, so a new
// recommendation REPLACES the outstanding target — repeated events cannot
// accumulate unbounded debt by construction.
//
// The controller never touches simulation: it only budgets presentation-side
// waits. Deterministic frames are never skipped or doubled to repay debt,
// and no game state may read this wall-clock bookkeeping.
//
// Pure component: no OS or game dependencies; the host performs the actual
// wait and reports what really elapsed (self-correcting under coarse timer
// granularity).

#include <stdint.h>

namespace sf4e {
namespace pacing {

struct PacingController {
	// Configuration (host may override from dev environment).
	double maxRecommendationFrames; // clamp on one recommendation (default 9)
	double maxStepMs;               // budget per outer frame (default 3.0)
	double minWaitMs;               // below this, don't bother waiting (1.0)
	bool enabled;                   // runtime A/B gate (default true)

	// State
	double outstandingMs;

	// Statistics
	uint32_t recommendationsReceived;
	uint64_t framesRecommendedTotal;
	double msAcceptedTotal;      // after clamping
	double msAppliedTotal;       // actually waited (as reported by host)
	double maxSingleWaitMs;      // largest single reported wait
	double maxOutstandingMs;     // high-water mark of debt
	double msDiscardedOnReset;   // debt thrown away by lifecycle resets
	double msReplacedTotal;      // outstanding debt replaced by a fresh estimate
	double msDiscardedDisabled;  // recommendations observed while A/B-disabled
	uint64_t waitRequests;
	double msRequestedTotal;
	double maxRequestedWaitMs;
	uint32_t waitFailures;
	uint32_t waitTimeouts;
	uint32_t fallbackSleeps;

	void ResetStats() {
		recommendationsReceived = 0;
		framesRecommendedTotal = 0;
		msAcceptedTotal = 0.0;
		msAppliedTotal = 0.0;
		maxSingleWaitMs = 0.0;
		maxOutstandingMs = 0.0;
		msDiscardedOnReset = 0.0;
		msReplacedTotal = 0.0;
		msDiscardedDisabled = 0.0;
		waitRequests = 0;
		msRequestedTotal = 0.0;
		maxRequestedWaitMs = 0.0;
		waitFailures = 0;
		waitTimeouts = 0;
		fallbackSleeps = 0;
	}

	void InitDefaults() {
		maxRecommendationFrames = 9.0;
		maxStepMs = 3.0;
		minWaitMs = 1.0;
		enabled = true;
		outstandingMs = 0.0;
		ResetStats();
	}

	// Lifecycle reset: new session, match close, rematch, terminal failure,
	// shutdown. Discards outstanding debt (recorded in stats).
	void Reset() {
		msDiscardedOnReset += outstandingMs;
		outstandingMs = 0.0;
	}

	// A GGPO timesync recommendation. Negative and zero are ignored.
	// The clamped fresh estimate REPLACES the outstanding target (see
	// header comment); it never sums.
	void OnRecommendation(int framesAhead) {
		recommendationsReceived++;
		if (framesAhead <= 0) {
			return;
		}
		framesRecommendedTotal += (uint64_t)framesAhead;
		double frames = (double)framesAhead;
		if (frames > maxRecommendationFrames) {
			frames = maxRecommendationFrames;
		}
		double ms = frames * (1000.0 / 60.0);
		if (!enabled) {
			msDiscardedDisabled += ms;
			return;
		}
		msReplacedTotal += outstandingMs;
		outstandingMs = ms;
		msAcceptedTotal += ms;
		if (outstandingMs > maxOutstandingMs) {
			maxOutstandingMs = outstandingMs;
		}
	}

	// How long the host should wait this outer frame (0 = don't wait).
	double NextWaitMs() const {
		if (!enabled || outstandingMs < minWaitMs) {
			return 0.0;
		}
		return outstandingMs < maxStepMs ? outstandingMs : maxStepMs;
	}

	void OnWaitRequested(double requestedMs) {
		if (requestedMs <= 0.0) {
			return;
		}
		waitRequests++;
		msRequestedTotal += requestedMs;
		if (requestedMs > maxRequestedWaitMs) {
			maxRequestedWaitMs = requestedMs;
		}
	}

	void OnWaitFailure(bool timeout) {
		waitFailures++;
		if (timeout) {
			waitTimeouts++;
		}
	}

	void OnFallbackSleep() {
		fallbackSleeps++;
	}

	// Host reports how long it actually waited (which may exceed the
	// request under coarse timers). Repays debt; never goes negative.
	void OnWaited(double actualMs) {
		if (actualMs <= 0.0) {
			return;
		}
		msAppliedTotal += actualMs;
		if (actualMs > maxSingleWaitMs) {
			maxSingleWaitMs = actualMs;
		}
		outstandingMs -= actualMs;
		if (outstandingMs < 0.0) {
			outstandingMs = 0.0;
		}
	}
};

} // namespace pacing
} // namespace sf4e
