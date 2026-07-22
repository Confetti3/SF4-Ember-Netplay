#pragma once

// Rollback/netplay diagnostics (Phase 1).
//
// A fixed-size, allocation-free telemetry component for the GGPO integration.
// Pure: no game, GGPO, or logging dependencies, so it can be unit tested and
// linked anywhere. Callers time operations with ScopedTimer (or Record*),
// and periodically emit FormatSummary through their own logger.
//
// Threading: all recording must happen on the game main thread (the same
// thread that runs BattleUpdate, Steam_PostUpdate, and every GGPO callback —
// see docs/GGPO_LIFECYCLE.md). This component is intentionally not
// synchronized.
//
// Enabled via SF4E_ROLLBACK_DIAGNOSTICS=1 (or SetEnabled from the host).
// When disabled every entry point is a single branch and no clock is read.

#include <stddef.h>
#include <stdint.h>

namespace sf4e {
namespace diag {

// ---- Timed operations -----------------------------------------------------

enum TimedOp {
	// Frame and network processing
	OP_DETOURED_BATTLE_UPDATE = 0, // entire fSystem::BattleUpdate
	OP_ENGINE_BATTLE_UPDATE,       // original (undetoured) engine update
	OP_ADD_LOCAL_INPUT,            // ggpo_add_local_input
	OP_SYNC_INPUT,                 // ggpo_synchronize_input
	OP_ADVANCE_FRAME_API,          // ggpo_advance_frame
	OP_ROLLBACK_CALLBACK,          // entire ggpo_advance_frame_callback
	OP_SESSION_CLIENT_STEP,        // SessionClient::Step
	OP_SESSION_SERVER_STEP,        // SessionServer::Step
	OP_FACADE_TICK_FRAME,          // NetplayFacade::TickFrame
	OP_GGPO_IDLE,                  // ggpo_idle
	OP_STEAM_POST_UPDATE,          // original (undetoured) Steam_PostUpdate
	OP_OUTER_TICK,                 // complete detoured Steam_PostUpdate

	// Rollback state operations
	OP_SAVE_TOTAL,                 // entire SaveState::Save
	OP_SAVE_RECORD_MEMENTOS,       // RecordAllToInternalMementos
	OP_SAVE_COPY_KEYS,             // copying tracked keys
	OP_SAVE_SOUND,                 // sound-state capture
	OP_SAVE_GLOBALS,               // global-data capture
	OP_SEMANTIC_HASH,              // periodic semantic checkpoint hashing
	OP_LOAD_TOTAL,                 // entire SaveState::Load
	OP_LOAD_KEY_BACKUP,            // temporary live-key backup
	OP_LOAD_COPY_INTO_PLACE,       // CopyIntoPlace + RestoreAllFromInternalMementos
	OP_LOAD_RESTORE_KEYS,          // zero injected keys + restore live keys
	OP_FREE_TOTAL,                 // entire SaveState::Free
	OP_FREE_TMP_SAVE,              // temporary live-state save inside Free
	OP_FREE_VICTIM_INSTALL,        // CopyIntoPlace(victim)
	OP_FREE_CLEAR,                 // victim-key cleanup
	OP_FREE_LIVE_RESTORE,          // live-state restoration

	// Blocking behavior we intend to remove (measured for before/after)
	OP_TIMESYNC_SLEEP,             // sleep inside the timesync event callback
	OP_PACING_WAIT,                // distributed pacing wait in the outer tick

	OP_COUNT
};

const char* TimedOpName(int op);

// ---- Histogram ------------------------------------------------------------

// Fixed hitch buckets (upper bounds, ms). The last bucket is open-ended.
static const int NUM_BUCKETS = 10;
extern const double kBucketUpperMs[NUM_BUCKETS]; // .25 .5 1 2 4 8 16.67 33 50 inf
static const int NUM_HITCH_THRESHOLDS = 5;
extern const double kHitchThresholdMs[NUM_HITCH_THRESHOLDS]; // 16.67 25 33.33 50 100

struct TimingStat {
	uint64_t count;
	double totalMs;
	double maxMs;
	double lastMs;
	uint32_t buckets[NUM_BUCKETS];
	uint64_t hitchCounts[NUM_HITCH_THRESHOLDS];

	void Reset();
	void Record(double ms);
	// Approximate percentile (0..1) from histogram buckets: returns the upper
	// bound of the bucket containing the requested rank (conservative).
	// Returns 0 when empty.
	double ApproxPercentileMs(double p) const;
	double MeanMs() const { return count ? totalMs / (double)count : 0.0; }
};

// ---- Skipped deterministic frames ----------------------------------------

enum SkipReason {
	SKIP_UPDATE_GATE = 0,      // bUpdateAllowed false (pause/interrupt/halt)
	SKIP_PREDICTION_THRESHOLD, // ggpo_add_local_input refused further prediction
	SKIP_NOT_SYNCHRONIZED,     // GGPO not synchronized yet
	SKIP_ADD_INPUT_ERROR,      // other ggpo_add_local_input failure
	SKIP_SYNC_INPUT_ERROR,     // ggpo_synchronize_input failure
	SKIP_COUNT
};

const char* SkipReasonName(int r);

// ---- GGPO return-code accounting ------------------------------------------

enum GgpoCallSite {
	CALL_ADD_LOCAL_INPUT = 0,
	CALL_SYNC_INPUT,
	CALL_ADVANCE_FRAME,
	CALL_GET_NETWORK_STATS,
	CALL_COUNT
};

// GGPO error codes span -1..11; slot 0 holds -1/other, slots 1..12 hold 0..11.
static const int NUM_RESULT_SLOTS = 13;

// ---- Gauges ----------------------------------------------------------------

struct Gauge {
	uint32_t current;
	uint32_t max;
	void Reset() { current = 0; max = 0; }
	void Update(uint32_t v) {
		current = v;
		if (v > max) max = v;
	}
};

// ---- Main component --------------------------------------------------------

struct RollbackDiagnostics {
	bool enabled;

	TimingStat ops[OP_COUNT];

	uint64_t skipReasons[SKIP_COUNT];
	uint32_t ggpoResults[CALL_COUNT][NUM_RESULT_SLOTS];

	// Rollback burst tracking. A "burst" is a run of consecutive rollback
	// callbacks not separated by a normally-advanced frame or an outer tick.
	// This is a PROXY for rollback work; it is NOT the exact rollback
	// distance (the callback interface does not expose origin/destination).
	uint64_t totalRollbackCallbacks;
	uint32_t rollbackCallbacksThisOuterFrame;
	uint32_t maxRollbackCallbacksPerOuterFrame;
	uint64_t outerFramesWithRollback;
	uint64_t outerFrames;
	uint32_t currentResimBurst;
	uint32_t maxResimBurst;
	// Burst-length histogram in frames; fixed upper bounds, last open-ended.
	static const int NUM_BURST_BUCKETS = 8; // 1,2,3,4,6,8,12,+
	uint32_t resimBurstBuckets[NUM_BURST_BUCKETS];
	uint64_t resimBursts;

	// Connection / stall events
	uint32_t connectionWarnings;
	uint32_t connectionResumes;
	uint32_t terminalDisconnects;
	double connectionWarningStartMs; // <0 when no warning active
	TimingStat connectionWarningDuration;

	uint32_t timesyncEvents;
	uint64_t timesyncFramesTotal;
	uint32_t timesyncMaxFrames;

	// A prediction-threshold stall episode: consecutive outer frames where
	// GGPO refused local input. Duration measured wall-clock from first
	// refusal to next successful advance.
	uint32_t predictionStalls;
	double predictionStallStartMs; // <0 when not stalled
	TimingStat predictionStallDuration;

	// State-size gauges (updated during save/load)
	Gauge trackedKeys;
	Gauge saveKeyVectorSize;
	Gauge saveKeyVectorCapacity;
	Gauge soundManagers;
	Gauge soundAdapters;
	Gauge criPlayerStateSize;
	Gauge managerStateSize;
	Gauge occupiedSaveSlots;

	double matchStartMs;
	double lastPeriodicEmitMs;

	// --- lifecycle ---
	void ResetForMatch(double nowMs);

	// --- recording (no-ops when disabled; callers may also pre-check) ---
	void RecordOp(int op, double ms);
	void RecordSkip(int reason, double nowMs);
	void RecordGgpoResult(int callSite, int ggpoErrorCode);
	void OnFrameAdvanced(double nowMs);   // a deterministic frame advanced normally
	void OnRollbackCallback(double nowMs);
	void OnOuterFrame(double nowMs);      // outer-tick accounting only
	void OnSessionEnded(double nowMs);    // closes active stall/burst episodes
	void OnConnectionInterrupted(double nowMs);
	void OnConnectionResumed(double nowMs);
	void OnTerminalDisconnect(double nowMs);
	void OnTimesyncEvent(int framesAhead);

	// --- output ---
	// True when a periodic dev summary should be emitted (every periodSec).
	bool PeriodicSummaryDue(double nowMs, double periodSec);
	// Formats a multi-line summary into buf; returns chars written (0 if
	// disabled or cap too small). Never allocates.
	size_t FormatSummary(char* buf, size_t cap, const char* label) const;
};

// Global instance + helpers ---------------------------------------------------

RollbackDiagnostics& G();

// Monotonic milliseconds (steady clock). Only call when enabled-checked or
// for lifecycle bookkeeping.
double NowMs();

// Reads SF4E_ROLLBACK_DIAGNOSTICS once and latches; host may force with
// SetEnabled (e.g. dev overlay builds).
void InitFromEnvironment();
void SetEnabled(bool enabled);
inline bool Enabled() { return G().enabled; }

// RAII timer: reads the clock only when diagnostics are enabled.
class ScopedTimer {
public:
	explicit ScopedTimer(int op) : _op(op), _armed(Enabled()), _t0(_armed ? NowMs() : 0.0) {}
	~ScopedTimer() {
		if (_armed) {
			G().RecordOp(_op, NowMs() - _t0);
		}
	}
	// Elapsed so far (0 when disabled); for callers that also branch on it.
	double ElapsedMs() const { return _armed ? NowMs() - _t0 : 0.0; }

private:
	ScopedTimer(const ScopedTimer&);
	ScopedTimer& operator=(const ScopedTimer&);
	int _op;
	bool _armed;
	double _t0;
};

} // namespace diag
} // namespace sf4e
