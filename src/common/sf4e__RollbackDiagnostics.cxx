#include "sf4e__RollbackDiagnostics.hxx"

#include <chrono>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sf4e {
namespace diag {

const double kBucketUpperMs[NUM_BUCKETS] = {
	0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.67, 33.0, 50.0, 1e300
};

static const uint32_t kBurstUpper[RollbackDiagnostics::NUM_BURST_BUCKETS] = {
	1, 2, 3, 4, 6, 8, 12, 0xffffffffu
};

const char* TimedOpName(int op) {
	switch (op) {
	case OP_DETOURED_BATTLE_UPDATE: return "battle_update";
	case OP_ENGINE_BATTLE_UPDATE:   return "engine_update";
	case OP_ADD_LOCAL_INPUT:        return "add_local_input";
	case OP_SYNC_INPUT:             return "sync_input";
	case OP_ADVANCE_FRAME_API:      return "advance_frame";
	case OP_ROLLBACK_CALLBACK:      return "rollback_cb";
	case OP_SESSION_CLIENT_STEP:    return "client_step";
	case OP_SESSION_SERVER_STEP:    return "server_step";
	case OP_FACADE_TICK_FRAME:      return "facade_tick";
	case OP_GGPO_IDLE:              return "ggpo_idle";
	case OP_STEAM_POST_UPDATE:      return "steam_post_update";
	case OP_SAVE_TOTAL:             return "save";
	case OP_SAVE_RECORD_MEMENTOS:   return "save.mementos";
	case OP_SAVE_COPY_KEYS:         return "save.keys";
	case OP_SAVE_SOUND:             return "save.sound";
	case OP_SAVE_GLOBALS:           return "save.globals";
	case OP_LOAD_TOTAL:             return "load";
	case OP_LOAD_KEY_BACKUP:        return "load.key_backup";
	case OP_LOAD_COPY_INTO_PLACE:   return "load.copy_into_place";
	case OP_LOAD_RESTORE_KEYS:      return "load.restore_keys";
	case OP_FREE_TOTAL:             return "free";
	case OP_FREE_TMP_SAVE:          return "free.tmp_save";
	case OP_FREE_VICTIM_INSTALL:    return "free.victim_install";
	case OP_FREE_CLEAR:             return "free.clear";
	case OP_FREE_LIVE_RESTORE:      return "free.live_restore";
	case OP_TIMESYNC_SLEEP:         return "timesync_sleep";
	case OP_PACING_WAIT:            return "pacing_wait";
	default:                        return "?";
	}
}

const char* SkipReasonName(int r) {
	switch (r) {
	case SKIP_UPDATE_GATE:          return "update_gate";
	case SKIP_PREDICTION_THRESHOLD: return "prediction_threshold";
	case SKIP_NOT_SYNCHRONIZED:     return "not_synchronized";
	case SKIP_ADD_INPUT_ERROR:      return "add_input_error";
	case SKIP_SYNC_INPUT_ERROR:     return "sync_input_error";
	default:                        return "?";
	}
}

static const char* CallSiteName(int c) {
	switch (c) {
	case CALL_ADD_LOCAL_INPUT:   return "add_local_input";
	case CALL_SYNC_INPUT:        return "sync_input";
	case CALL_ADVANCE_FRAME:     return "advance_frame";
	case CALL_GET_NETWORK_STATS: return "get_network_stats";
	default:                     return "?";
	}
}

// ---- TimingStat ------------------------------------------------------------

void TimingStat::Reset() {
	count = 0;
	totalMs = 0.0;
	maxMs = 0.0;
	lastMs = 0.0;
	memset(buckets, 0, sizeof(buckets));
}

void TimingStat::Record(double ms) {
	if (ms < 0.0) {
		ms = 0.0;
	}
	count++;
	totalMs += ms;
	lastMs = ms;
	if (ms > maxMs) {
		maxMs = ms;
	}
	for (int i = 0; i < NUM_BUCKETS; i++) {
		if (ms < kBucketUpperMs[i]) {
			buckets[i]++;
			return;
		}
	}
	buckets[NUM_BUCKETS - 1]++;
}

double TimingStat::ApproxPercentileMs(double p) const {
	if (count == 0) {
		return 0.0;
	}
	if (p < 0.0) p = 0.0;
	if (p > 1.0) p = 1.0;
	uint64_t rank = (uint64_t)(p * (double)count);
	if (rank >= count) {
		rank = count - 1;
	}
	uint64_t cumulative = 0;
	for (int i = 0; i < NUM_BUCKETS; i++) {
		cumulative += buckets[i];
		if (rank < cumulative) {
			// Open-ended final bucket: report the observed max instead of inf.
			if (i == NUM_BUCKETS - 1) {
				return maxMs;
			}
			return kBucketUpperMs[i];
		}
	}
	return maxMs;
}

// ---- RollbackDiagnostics ---------------------------------------------------

void RollbackDiagnostics::ResetForMatch(double nowMs) {
	bool wasEnabled = enabled;
	memset(this, 0, sizeof(*this));
	enabled = wasEnabled;
	connectionWarningStartMs = -1.0;
	predictionStallStartMs = -1.0;
	matchStartMs = nowMs;
	lastPeriodicEmitMs = nowMs;
}

void RollbackDiagnostics::RecordOp(int op, double ms) {
	if (!enabled || op < 0 || op >= OP_COUNT) {
		return;
	}
	ops[op].Record(ms);
}

void RollbackDiagnostics::RecordSkip(int reason, double nowMs) {
	if (!enabled || reason < 0 || reason >= SKIP_COUNT) {
		return;
	}
	skipReasons[reason]++;
	if (reason == SKIP_PREDICTION_THRESHOLD && predictionStallStartMs < 0.0) {
		predictionStalls++;
		predictionStallStartMs = nowMs;
	}
}

void RollbackDiagnostics::RecordGgpoResult(int callSite, int code) {
	if (!enabled || callSite < 0 || callSite >= CALL_COUNT) {
		return;
	}
	// Slot 0 collects -1 and out-of-range codes; 1..12 map codes 0..11.
	int slot = (code >= 0 && code <= 11) ? code + 1 : 0;
	ggpoResults[callSite][slot]++;
}

void RollbackDiagnostics::OnFrameAdvanced(double nowMs) {
	if (!enabled) {
		return;
	}
	if (predictionStallStartMs >= 0.0) {
		predictionStallDuration.Record(nowMs - predictionStallStartMs);
		predictionStallStartMs = -1.0;
	}
	// A normal advance ends any resimulation burst.
	if (currentResimBurst > 0) {
		if (currentResimBurst > maxResimBurst) {
			maxResimBurst = currentResimBurst;
		}
		resimBursts++;
		for (int i = 0; i < NUM_BURST_BUCKETS; i++) {
			if (currentResimBurst <= kBurstUpper[i]) {
				resimBurstBuckets[i]++;
				break;
			}
		}
		currentResimBurst = 0;
	}
}

void RollbackDiagnostics::OnRollbackCallback(double nowMs) {
	(void)nowMs;
	if (!enabled) {
		return;
	}
	totalRollbackCallbacks++;
	rollbackCallbacksThisOuterFrame++;
	currentResimBurst++;
}

void RollbackDiagnostics::OnOuterFrame(double nowMs) {
	if (!enabled) {
		return;
	}
	outerFrames++;
	if (rollbackCallbacksThisOuterFrame > 0) {
		outerFramesWithRollback++;
		if (rollbackCallbacksThisOuterFrame > maxRollbackCallbacksPerOuterFrame) {
			maxRollbackCallbacksPerOuterFrame = rollbackCallbacksThisOuterFrame;
		}
		rollbackCallbacksThisOuterFrame = 0;
	}
	// An outer tick also bounds a resim burst (rollback resimulation never
	// spans outer frames).
	OnFrameAdvanced(nowMs);
}

void RollbackDiagnostics::OnConnectionInterrupted(double nowMs) {
	if (!enabled) {
		return;
	}
	if (connectionWarningStartMs < 0.0) {
		connectionWarnings++;
		connectionWarningStartMs = nowMs;
	}
}

void RollbackDiagnostics::OnConnectionResumed(double nowMs) {
	if (!enabled) {
		return;
	}
	connectionResumes++;
	if (connectionWarningStartMs >= 0.0) {
		connectionWarningDuration.Record(nowMs - connectionWarningStartMs);
		connectionWarningStartMs = -1.0;
	}
}

void RollbackDiagnostics::OnTerminalDisconnect(double nowMs) {
	if (!enabled) {
		return;
	}
	terminalDisconnects++;
	if (connectionWarningStartMs >= 0.0) {
		connectionWarningDuration.Record(nowMs - connectionWarningStartMs);
		connectionWarningStartMs = -1.0;
	}
}

void RollbackDiagnostics::OnTimesyncEvent(int framesAhead) {
	if (!enabled) {
		return;
	}
	timesyncEvents++;
	if (framesAhead > 0) {
		timesyncFramesTotal += (uint64_t)framesAhead;
		if ((uint32_t)framesAhead > timesyncMaxFrames) {
			timesyncMaxFrames = (uint32_t)framesAhead;
		}
	}
}

bool RollbackDiagnostics::PeriodicSummaryDue(double nowMs, double periodSec) {
	if (!enabled) {
		return false;
	}
	if (nowMs - lastPeriodicEmitMs >= periodSec * 1000.0) {
		lastPeriodicEmitMs = nowMs;
		return true;
	}
	return false;
}

// Append helper: snprintf into remaining space, saturating.
static void Append(char* buf, size_t cap, size_t* used, const char* fmt, ...) {
	if (*used >= cap) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(buf + *used, cap - *used, fmt, args);
	va_end(args);
	if (n > 0) {
		size_t adv = (size_t)n;
		*used += (adv < cap - *used) ? adv : (cap - *used - 1);
	}
}

static void AppendStatLine(
	char* buf, size_t cap, size_t* used, const char* name, const TimingStat& s
) {
	if (s.count == 0) {
		return;
	}
	Append(
		buf, cap, used,
		"  %-22s n=%llu p50=%.2f p95=%.2f p99=%.2f max=%.2f mean=%.3f ms\n",
		name,
		(unsigned long long)s.count,
		s.ApproxPercentileMs(0.50),
		s.ApproxPercentileMs(0.95),
		s.ApproxPercentileMs(0.99),
		s.maxMs,
		s.MeanMs()
	);
}

size_t RollbackDiagnostics::FormatSummary(char* buf, size_t cap, const char* label) const {
	if (!enabled || !buf || cap < 2) {
		return 0;
	}
	size_t used = 0;
	buf[0] = '\0';

	Append(buf, cap, &used, "RollbackDiag [%s] outerFrames=%llu\n",
		label ? label : "-", (unsigned long long)outerFrames);

	// -- Hard stalls: frames where deterministic advancement was refused or
	//    the callback path intentionally blocked.
	Append(buf, cap, &used, " hard stalls:\n");
	for (int i = 0; i < SKIP_COUNT; i++) {
		if (skipReasons[i]) {
			Append(buf, cap, &used, "  skip[%s]=%llu\n",
				SkipReasonName(i), (unsigned long long)skipReasons[i]);
		}
	}
	Append(buf, cap, &used,
		"  prediction stalls=%u (active=%s)\n",
		predictionStalls, predictionStallStartMs >= 0.0 ? "yes" : "no");
	AppendStatLine(buf, cap, &used, "stall_duration", predictionStallDuration);
	Append(buf, cap, &used,
		"  conn warnings=%u resumes=%u terminal=%u\n",
		connectionWarnings, connectionResumes, terminalDisconnects);
	AppendStatLine(buf, cap, &used, "warning_duration", connectionWarningDuration);
	Append(buf, cap, &used,
		"  timesync events=%u framesTotal=%llu max=%u\n",
		timesyncEvents, (unsigned long long)timesyncFramesTotal, timesyncMaxFrames);
	AppendStatLine(buf, cap, &used, "timesync_sleep", ops[OP_TIMESYNC_SLEEP]);
	AppendStatLine(buf, cap, &used, "pacing_wait", ops[OP_PACING_WAIT]);

	// -- Simulation hitches: cost of work inside an outer frame.
	Append(buf, cap, &used, " simulation hitches:\n");
	static const int hitchOps[] = {
		OP_DETOURED_BATTLE_UPDATE, OP_ENGINE_BATTLE_UPDATE, OP_ADD_LOCAL_INPUT,
		OP_SYNC_INPUT, OP_ADVANCE_FRAME_API, OP_ROLLBACK_CALLBACK,
		OP_SESSION_CLIENT_STEP, OP_SESSION_SERVER_STEP, OP_FACADE_TICK_FRAME,
		OP_GGPO_IDLE, OP_STEAM_POST_UPDATE,
		OP_SAVE_TOTAL, OP_SAVE_RECORD_MEMENTOS, OP_SAVE_COPY_KEYS,
		OP_SAVE_SOUND, OP_SAVE_GLOBALS,
		OP_LOAD_TOTAL, OP_LOAD_KEY_BACKUP, OP_LOAD_COPY_INTO_PLACE,
		OP_LOAD_RESTORE_KEYS,
		OP_FREE_TOTAL, OP_FREE_TMP_SAVE, OP_FREE_VICTIM_INSTALL,
		OP_FREE_CLEAR, OP_FREE_LIVE_RESTORE,
	};
	for (size_t i = 0; i < sizeof(hitchOps) / sizeof(hitchOps[0]); i++) {
		AppendStatLine(buf, cap, &used, TimedOpName(hitchOps[i]), ops[hitchOps[i]]);
	}

	// -- Visible corrections: resimulation bursts (PROXY for rollback
	//    distance, not exact origin/destination).
	Append(buf, cap, &used, " visible corrections (burst = proxy, not exact distance):\n");
	Append(buf, cap, &used,
		"  rollback callbacks=%llu outerFramesWithRollback=%llu maxPerOuterFrame=%u\n",
		(unsigned long long)totalRollbackCallbacks,
		(unsigned long long)outerFramesWithRollback,
		maxRollbackCallbacksPerOuterFrame);
	Append(buf, cap, &used, "  resim bursts=%llu maxBurst=%u buckets[<=1,2,3,4,6,8,12,+]=",
		(unsigned long long)resimBursts, maxResimBurst);
	for (int i = 0; i < NUM_BURST_BUCKETS; i++) {
		Append(buf, cap, &used, "%s%u", i ? "," : "", resimBurstBuckets[i]);
	}
	Append(buf, cap, &used, "\n");

	// -- GGPO return codes
	Append(buf, cap, &used, " ggpo results (slot0=other/-1, slotN=code N-1):\n");
	for (int c = 0; c < CALL_COUNT; c++) {
		uint64_t total = 0;
		for (int s = 0; s < NUM_RESULT_SLOTS; s++) {
			total += ggpoResults[c][s];
		}
		if (!total) {
			continue;
		}
		Append(buf, cap, &used, "  %-18s", CallSiteName(c));
		for (int s = 0; s < NUM_RESULT_SLOTS; s++) {
			if (ggpoResults[c][s]) {
				Append(buf, cap, &used, " [%d]=%u", s - 1, ggpoResults[c][s]);
			}
		}
		Append(buf, cap, &used, "\n");
	}

	// -- State sizes
	Append(buf, cap, &used,
		" state sizes (cur/max): trackedKeys=%u/%u keyVec=%u/%u keyCap=%u/%u "
		"sndMgr=%u/%u sndAdpt=%u/%u criState=%u/%u mgrState=%u/%u slots=%u/%u\n",
		trackedKeys.current, trackedKeys.max,
		saveKeyVectorSize.current, saveKeyVectorSize.max,
		saveKeyVectorCapacity.current, saveKeyVectorCapacity.max,
		soundManagers.current, soundManagers.max,
		soundAdapters.current, soundAdapters.max,
		criPlayerStateSize.current, criPlayerStateSize.max,
		managerStateSize.current, managerStateSize.max,
		occupiedSaveSlots.current, occupiedSaveSlots.max);

	return used;
}

// ---- Globals ---------------------------------------------------------------

static RollbackDiagnostics s_diag; // zero-initialized static storage

RollbackDiagnostics& G() {
	return s_diag;
}

double NowMs() {
	using namespace std::chrono;
	return duration_cast<duration<double, std::milli>>(
		steady_clock::now().time_since_epoch()
	).count();
}

void InitFromEnvironment() {
	const char* env = getenv("SF4E_ROLLBACK_DIAGNOSTICS");
	if (env && env[0] == '1') {
		s_diag.enabled = true;
	}
	s_diag.connectionWarningStartMs = -1.0;
	s_diag.predictionStallStartMs = -1.0;
}

void SetEnabled(bool enabled) {
	s_diag.enabled = enabled;
	if (s_diag.connectionWarningStartMs == 0.0) {
		s_diag.connectionWarningStartMs = -1.0;
	}
	if (s_diag.predictionStallStartMs == 0.0) {
		s_diag.predictionStallStartMs = -1.0;
	}
}

} // namespace diag
} // namespace sf4e
