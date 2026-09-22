#pragma once

// Time-sync pacing controller.
//
// Keeps a signed correction target in milliseconds: positive means this side
// should run slower, negative faster. The host repays it in small slices by
// lengthening or shortening outer frames, then reports what each frame really
// changed. Two sources feed the target, chosen once per session:
//
// * continuous: OnRiftSample takes the frame-advantage pair every outer tick;
// * coarse: OnRecommendation takes GGPO's timesync event. Semantics verified
//   against the pinned fork (adanducci/ggpo@c88b667, timesync.cpp): a
//   recommendation is a FRESH estimate of how many frames we are ahead (half
//   the averaged advantage delta, clamped to MAX_FRAME_ADVANTAGE=9, gated at
//   MIN_FRAME_ADVANTAGE=3, emitted at most once per 240 frames), so it
//   REPLACES the target and repeated events cannot accumulate.
//
// The controller never touches simulation: it only budgets how long outer
// frames last. Deterministic frames are never skipped or doubled to repay
// debt, and no game state may read this wall-clock bookkeeping.
//
// Pure component: no OS or game dependencies.

#include <stdint.h>

namespace sf4e {
namespace pacing {

struct PacingController {
	// Configuration (host may override from dev environment).
	double maxRecommendationFrames; // clamp on one recommendation (default 9)
	double maxStepMs;               // most one frame may change (default 3.0)
	double minShiftMs;              // below this, don't bother shifting (1.0)
	bool enabled;                   // runtime A/B gate (default true)
	bool continuous;                // rift samples (true) or GGPO events (false, default)

	// State: positive = run slower by this much, negative = run faster.
	double outstandingMs;

	// Statistics
	uint32_t recommendationsReceived;
	uint64_t framesRecommendedTotal;
	double msAcceptedTotal;      // after clamping
	double msSlowedTotal;        // frames actually lengthened (as reported by host)
	double msSpedUpTotal;        // frames actually shortened (as reported by host)
	double maxSingleShiftMs;     // largest single reported change, either way
	double maxOutstandingMs;     // high-water mark of |debt|
	double msDiscardedOnReset;   // |debt| thrown away by lifecycle resets
	double msReplacedTotal;      // outstanding debt replaced by a fresh estimate
	double msDiscardedDisabled;  // recommendations observed while A/B-disabled

	// Continuous rift correction. The coarse GGPO event fires at most once per
	// 240 frames and ignores a rift under 3 frames; OnRiftSample instead repays
	// a fraction of a frame every tick.
	double riftSmoothing;     // EMA weight per sample (default 1/15)
	double riftDeadZoneFrames; // no correction below this (default 0.75)
	double riftGain;          // fraction of the excess rift repaid per tick, per side (1/120)
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
		msSlowedTotal = 0.0;
		msSpedUpTotal = 0.0;
		maxSingleShiftMs = 0.0;
		maxOutstandingMs = 0.0;
		msDiscardedOnReset = 0.0;
		msReplacedTotal = 0.0;
		msDiscardedDisabled = 0.0;
		riftSamples = 0;
		msRiftAcceptedTotal = 0.0;
		maxAbsRiftFrames = 0.0;
	}

	void InitDefaults() {
		maxRecommendationFrames = 9.0;
		maxStepMs = 3.0;
		minShiftMs = 1.0;
		enabled = true;
		continuous = false;
		outstandingMs = 0.0;
		riftSmoothing = 1.0 / 15.0;
		riftDeadZoneFrames = 0.75;
		riftGain = 1.0 / 120.0;
		riftHoldTicks = 45;
		riftHoldRemaining = 0;
		riftFramesEma = 0.0;
		hasRift = false;
		ResetStats();
	}

	// Lifecycle reset: new session, match close, rematch, terminal failure,
	// shutdown. Discards outstanding debt (recorded in stats).
	void Reset() {
		msDiscardedOnReset += Abs(outstandingMs);
		outstandingMs = 0.0;
		riftFramesEma = 0.0;
		hasRift = false;
		riftHoldRemaining = 0;
	}

	// A GGPO timesync recommendation; only used in coarse mode. Negative and
	// zero are ignored. The clamped fresh estimate REPLACES the target.
	void OnRecommendation(int framesAhead) {
		recommendationsReceived++;
		if (continuous || framesAhead <= 0) {
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
		NoteOutstanding();
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
	// difference is how far ahead we run. The side that is ahead slows down
	// and the side that is behind speeds up, so each closes half the gap.
	// Both sides together repay the excess over about a second, slower than
	// the EMA, so delayed feedback cannot overshoot; the cap keeps a stale
	// lump from building while shifts are blocked. In coarse mode the rift is
	// only measured.
	void OnRiftSample(double localFramesBehind, double remoteFramesBehind) {
		if (riftHoldRemaining > 0) {
			--riftHoldRemaining;
			return;
		}
		const double rift = (remoteFramesBehind - localFramesBehind) * 0.5;
		riftFramesEma = hasRift ? riftFramesEma + (rift - riftFramesEma) * riftSmoothing : rift;
		hasRift = true;
		riftSamples++;
		if (Abs(riftFramesEma) > maxAbsRiftFrames) {
			maxAbsRiftFrames = Abs(riftFramesEma);
		}
		if (!enabled || !continuous || Abs(riftFramesEma) <= riftDeadZoneFrames) {
			return;
		}
		const double excess = riftFramesEma > 0.0 ? riftFramesEma - riftDeadZoneFrames : riftFramesEma + riftDeadZoneFrames;
		const double before = outstandingMs;
		outstandingMs = Clamp(outstandingMs + excess * (1000.0 / 60.0) * riftGain, 2.0 * maxStepMs);
		msRiftAcceptedTotal += Abs(outstandingMs - before);
		NoteOutstanding();
	}

	// How much to change the next outer frame: positive lengthens, negative
	// shortens, 0 leaves it alone.
	double NextShiftMs() const {
		if (!enabled || Abs(outstandingMs) < minShiftMs) {
			return 0.0;
		}
		return Clamp(outstandingMs, maxStepMs);
	}

	// Host reports what a frame really changed, signed like NextShiftMs. The
	// debt moves by exactly that much, so a shift still in flight when the
	// target changes sign is still accounted for.
	void OnShiftApplied(double ms) {
		if (ms > 0.0) {
			msSlowedTotal += ms;
		}
		else {
			msSpedUpTotal -= ms;
		}
		if (Abs(ms) > maxSingleShiftMs) {
			maxSingleShiftMs = Abs(ms);
		}
		outstandingMs -= ms;
	}

private:
	static double Abs(double v) { return v < 0.0 ? -v : v; }
	static double Clamp(double v, double limit) { return v > limit ? limit : v < -limit ? -limit : v; }
	void NoteOutstanding() {
		if (Abs(outstandingMs) > maxOutstandingMs) {
			maxOutstandingMs = Abs(outstandingMs);
		}
	}
};

// The game's FIXED limiter spins until one period after its previous exit.
// The host lengthens (shiftMs > 0) or shortens (< 0) that period for one frame.
// Shortening is limited to the time still left before the deadline, less a
// margin: an overrun would feed the engine's own lag catch-up instead.
inline double ShiftedPeriodMs(double periodMs, double elapsedMs, double shiftMs, double marginMs = 0.25) {
	if (shiftMs >= 0.0) {
		return periodMs + shiftMs;
	}
	double slackMs = periodMs - elapsedMs - marginMs;
	if (slackMs < 0.0) {
		slackMs = 0.0;
	}
	return periodMs - (-shiftMs < slackMs ? -shiftMs : slackMs);
}

// What a shifted frame really changed, from its measured length: never more
// than requested, never the wrong sign. A frame that ran long for another
// reason still counts toward a slowdown, since it slowed the clock too.
inline double AppliedShiftMs(double periodMs, double frameMs, double shiftMs) {
	double applied = shiftMs >= 0.0 ? frameMs - periodMs : periodMs - frameMs;
	const double limit = shiftMs >= 0.0 ? shiftMs : -shiftMs;
	if (applied < 0.0) {
		applied = 0.0;
	}
	if (applied > limit) {
		applied = limit;
	}
	return shiftMs >= 0.0 ? applied : -applied;
}

} // namespace pacing
} // namespace sf4e
