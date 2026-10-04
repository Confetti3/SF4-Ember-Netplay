#include "soak_metrics.hxx"
#include <psapi.h>
#include <iomanip>

namespace sf4e { namespace test { namespace soak {

// ---- Process accounting ----------------------------------------------------

struct Memory { std::size_t helpers = 0; std::uint64_t workingSet = 0; };
Memory HelperMemory(const Room& room) {
	Memory total;
	for (const auto& member : room.members) {
		if (!member->s || !member->s->process.IsRunning()) continue;
		++total.helpers;
		if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, member->s->process.Bootstrap().helperPid)) {
			PROCESS_MEMORY_COUNTERS counters = {sizeof(counters)};
			if (GetProcessMemoryInfo(process, &counters, sizeof(counters))) total.workingSet += counters.WorkingSetSize;
			CloseHandle(process);
		}
	}
	return total;
}
double Mb(std::uint64_t bytes) { return bytes / 1048576.0; }
std::uint64_t SoakWorkingSet() {
	PROCESS_MEMORY_COUNTERS counters = {sizeof(counters)};
	return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.WorkingSetSize : 0;
}
std::uint64_t ProcessTime(HANDLE process) {
	FILETIME created, exited, kernel, user;
	if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
	return ((static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
		(static_cast<std::uint64_t>(user.dwHighDateTime) << 32 | user.dwLowDateTime)) / 10000;
}
// The room's helpers' processor time since the last call, as percent of one core.
double RoomHelperCpu(Room& room, Clock now) {
	std::uint64_t used = 0;
	for (const auto& member : room.members) {
		if (!member->s || !member->s->process.IsRunning()) continue;
		const DWORD pid = member->s->process.Bootstrap().helperPid;
		if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
			const auto total = ProcessTime(process);
			used += total - (std::min)(total, room.cpuLast[pid]);
			room.cpuLast[pid] = total;
			CloseHandle(process);
		}
	}
	const double wall = static_cast<double>(Elapsed(now, room.cpuAt));
	room.cpuAt = now;
	return wall > 0 ? 100.0 * used / wall : 0;
}

std::mutex csvMutex;
std::atomic<double> processCpuPct{0};
std::vector<Room*> allRooms;

const char* const CsvHeader =
	"time_utc,elapsed_s,room,members_active,members_target,view_members_min,view_members_max,behind_members,"
	"joins_total,rejoins_total,join_failures_total,refused_total,degraded_total,control_losses_total,helper_crashes_total,"
	"join_ms_avg,join_ms_max,actions_sent_total,actions_accepted_total,actions_rejected_total,action_timeouts_total,send_failures_total,"
	"client_errors_total,rejects_by_reason,join_failures_by_reason,control_losses_by_reason,"
	"chat_sent_total,chat_sampled_total,chat_seen_total,chat_lost_total,chat_lines_checked_total,chat_lines_missing_total,chat_rtt_avg_ms,chat_rtt_max_ms,"
	"stale_avg_ms,stale_max_ms,probes_ok_total,probes_failed_total,matches_started_total,matches_ok_total,matches_failed_total,"
	"match_failures_by_step,fighter_changes_total,fighter_unseen_total,"
	"room_helpers,room_helpers_ws_mb,room_helpers_cpu_pct,all_helpers,all_helpers_ws_mb,all_helpers_cpu_pct,soak_ws_mb,client_cpu_pct,"
	"loop_avg_ms,loop_max_ms,loop_busy_pct,"
	"backlog_msgs_max,backlog_bytes_max,staged_checkpoints_max,helper_lag_ms_max,helper_silent_ms_max";

void WriteRow(std::ofstream& csv, Room& room, Clock now) {
	auto& st = room.st;
	room.CheckChat(now);
	std::size_t viewMin = SIZE_MAX, viewMax = 0, behind = 0;
	for (auto& member : room.members) {
		if (!member->IsActive()) continue;
		viewMin = (std::min)(viewMin, member->View().members.size());
		viewMax = (std::max)(viewMax, member->View().members.size());
		if (member->behindSince && Elapsed(now, member->behindSince) > 5000) ++behind;
	}
	if (viewMin == SIZE_MAX) viewMin = 0;
	const auto mine = HelperMemory(room);
	const double helperCpu = RoomHelperCpu(room, now);
	room.helpersNow = mine.helpers; room.helpersWs = mine.workingSet; room.helpersCpuPct = helperCpu;
	std::uint64_t allHelpers = 0, allWs = 0;
	double allCpu = 0;
	for (const auto* other : allRooms) { allHelpers += other->helpersNow; allWs += other->helpersWs; allCpu += other->helpersCpuPct; }
	const auto active = room.ActiveCount();
	const double fraction = static_cast<double>(active) / room.members.size();
	const double busy = 100.0 * room.loop.sum / (std::max<Clock>)(1, Elapsed(now, room.lastRowAt));
	++room.rows;
	room.fractionSum += fraction;
	if (busy > 90) ++room.saturatedRows;
	if (fraction >= 0.5) room.reachedHalf = true;
	room.emptyRows = active == 0 ? room.emptyRows + 1 : 0;
	if (room.reachedHalf && room.emptyRows >= 3 && !room.lost) { room.lost = true; Event(room.number, -1, "ROOM LOST: no member connected for 3 minutes"); }
	if (!room.reachedHalf && Elapsed(now, room.started) > 15 * 60000ull && !room.lost) { room.lost = true; Event(room.number, -1, "ROOM LOST: never filled to half in 15 minutes"); }
	std::lock_guard<std::mutex> lock(csvMutex);
	csv << std::fixed << std::setprecision(1) << UtcStamp() << ',' << Elapsed(now, runStart) / 1000 << ',' << room.number << ',' << active << ',' << room.members.size()
		<< ',' << viewMin << ',' << viewMax << ',' << behind
		<< ',' << st.joins << ',' << st.rejoins << ',' << st.joinFailures << ',' << st.refused << ',' << st.degraded << ',' << st.controlLosses << ',' << st.helperCrashes
		<< ',' << Average(st.joinMs) << ',' << Maximum(st.joinMs)
		<< ',' << st.actionsSent << ',' << st.actionsAccepted << ',' << st.actionsRejected << ',' << st.actionTimeouts << ',' << st.sendFailures
		<< ',' << st.clientErrors << ",\"" << Flat(st.rejects) << "\",\"" << Flat(st.joinFailureReasons) << "\",\"" << Flat(st.lossReasons) << '"'
		<< ',' << st.chatSent << ',' << st.chatSampled << ',' << st.chatSeen << ',' << st.chatLost << ',' << st.chatChecked << ',' << st.chatMissing << ',' << Average(st.chatRtt) << ',' << Maximum(st.chatRtt)
		<< ',' << (st.staleCount ? static_cast<double>(st.staleSum) / st.staleCount : 0.0) << ',' << st.staleMax
		<< ',' << st.probesOk << ',' << st.probesFailed << ',' << st.matchesStarted << ',' << st.matchesOk << ',' << st.matchesFailed
		<< ",\"" << Flat(st.matchFailureSteps) << '"' << ',' << st.fighterSent << ',' << st.fighterUnseen
		<< ',' << mine.helpers << ',' << Mb(mine.workingSet) << ',' << helperCpu << ',' << allHelpers << ',' << Mb(allWs) << ',' << allCpu
		<< ',' << Mb(SoakWorkingSet()) << ',' << processCpuPct.load()
		<< ',' << (room.loop.count ? static_cast<double>(room.loop.sum) / room.loop.count : 0.0) << ',' << room.loop.max << ',' << busy
		<< ',' << st.queueMax << ',' << st.queueBytesMax << ',' << st.stagedMax << ',' << st.helperLagMaxMs << ',' << st.helperSilentMaxMs << std::endl;
	st.joinMs.clear(); st.chatRtt.clear();
	st.staleSum = st.staleCount = st.staleMax = 0;
	st.queueMax = st.queueBytesMax = st.stagedMax = st.helperLagMaxMs = st.helperSilentMaxMs = 0;
	room.loop = {};
	room.lastRowAt = now;
}

} } }
