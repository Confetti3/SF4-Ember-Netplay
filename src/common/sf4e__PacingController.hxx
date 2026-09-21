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

	// Continuous rift correction. The coarse GGPO event fires at most once per
	// 240 frames and ignores a rift under 3 frames; OnRiftSample instead takes
	// the frame-advantage pair every outer tick and repays a fraction of a
	// frame continuously. The host feeds one source or the other, never both.
	double riftSmoothing;     // EMA weight per sample (default 1/15)
	double riftDeadZoneFrames; // no correction below this (default 0.75)
	double riftGain;          // fraction of the excess rift repaid per tick (1/60)
	int riftHoldTicks;        // samples ignored after a prediction stall (default 45)
	int riftHoldRemaining;    // state
	double riftFramesEma;     // state: smoothed frames we are ahead (+) or behind (-)
	bool hasRift;
	uint64_t riftSamples;
	double msRiftAcceptedTotal;
	double maxAbsRiftFrames;

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
		riftSamples = 0;
		msRiftAcceptedTotal = 0.0;
		maxAbsRiftFrames = 0.0;
	}

	void InitDefaults() {
		maxRecommendationFrames = 9.0;
		maxStepMs = 3.0;
		minWaitMs = 1.0;
		enabled = true;
		outstandingMs = 0.0;
		riftSmoothing = 1.0 / 15.0;
		riftDeadZoneFrames = 0.75;
		riftGain = 1.0 / 60.0;
		riftHoldTicks = 45;
		riftHoldRemaining = 0;
		riftFramesEma = 0.0;
		hasRift = false;
		ResetStats();
	}

	// Lifecycle reset: new session, match close, rematch, terminal failure,
	// shutdown. Discards outstanding debt (recorded in stats).
	void Reset() {
		msDiscardedOnReset += outstandingMs;
		outstandingMs = 0.0;
		riftFramesEma = 0.0;
		hasRift = false;
		riftHoldRemaining = 0;
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

	// Call on every tick GGPO refuses input at the prediction barrier. While
	// stalled and for a while after, the last received frame is stale and the
	// advantage pair is off by several frames (measured: up to 7, against
	// under 1 when calm), so those samples are dropped and the average holds.
	void OnPredictionStall() {
		riftHoldRemaining = riftHoldTicks;
	}

	// One sample per eligible outer tick from ggpo_get_network_stats. Both
	// values are "frames behind the peer" as seen by each side, so half the
	// difference is how far ahead we run. Only the side that is ahead waits.
	// The gain repays the excess over about a second, slower than the EMA, so
	// delayed feedback cannot overshoot; the cap keeps a stale lump from
	// building while waits are blocked.
	void OnRiftSample(double localFramesBehind, double remoteFramesBehind) {
		if (riftHoldRemaining > 0) {
			--riftHoldRemaining;
			return;
		}
		const double rift = (remoteFramesBehind - localFramesBehind) * 0.5;
		riftFramesEma = hasRift ? riftFramesEma + (rift - riftFramesEma) * riftSmoothing : rift;
		hasRift = true;
		riftSamples++;
		const double absRift = riftFramesEma < 0.0 ? -riftFramesEma : riftFramesEma;
		if (absRift > maxAbsRiftFrames) {
			maxAbsRiftFrames = absRift;
		}
		if (!enabled || riftFramesEma <= riftDeadZoneFrames) {
			return;
		}
		double ms = (riftFramesEma - riftDeadZoneFrames) * (1000.0 / 60.0) * riftGain;
		const double room = 2.0 * maxStepMs - outstandingMs;
		if (ms > room) {
			ms = room;
		}
		if (ms <= 0.0) {
			return;
		}
		outstandingMs += ms;
		msRiftAcceptedTotal += ms;
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
