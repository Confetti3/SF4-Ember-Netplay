#pragma once

// The soak's counters and the arithmetic on them. Plain standard C++ with no
// Windows or session headers, so the verdict can be checked on made-up numbers
// (SoakVerdictTest).
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sf4e { namespace test { namespace soak {

using Clock = std::uint64_t; // GetTickCount64 milliseconds

// The time from since to now, or 0 when since is the later one. Every elapsed
// time in the soak goes through here: its stamps come from different points of
// a loop pass (the pass's own time, a fresh Now(), a helper's receive time),
// and a plain unsigned difference of a later stamp wraps to a huge age.
inline Clock Elapsed(Clock now, Clock since) { return now > since ? now - since : 0; }

using Counts = std::map<std::string, std::uint64_t>;
inline std::string Flat(const Counts& counts) {
	std::string out;
	for (const auto& entry : counts) out += (out.empty() ? "" : ";") + entry.first + "=" + std::to_string(entry.second);
	return out;
}

inline double Percentile(std::vector<std::uint64_t> values, double fraction) {
	if (values.empty()) return 0;
	std::sort(values.begin(), values.end());
	return static_cast<double>(values[(std::min)(values.size() - 1, static_cast<std::size_t>(fraction * values.size()))]);
}
inline double Average(const std::vector<std::uint64_t>& values) {
	if (values.empty()) return 0;
	double sum = 0;
	for (const auto value : values) sum += static_cast<double>(value);
	return sum / values.size();
}
inline std::uint64_t Maximum(const std::vector<std::uint64_t>& values) { return values.empty() ? 0 : *std::max_element(values.begin(), values.end()); }

struct Stats {
	// controlLosses counts every member dropped, whatever the cause (lossReasons
	// says which). helperCrashes is the part of it where the helper process had
	// exited: a subset for diagnosis, never added to controlLosses.
	std::uint64_t joins = 0, rejoins = 0, joinFailures = 0, refused = 0, degraded = 0, controlLosses = 0, helperCrashes = 0;
	std::uint64_t actionsSent = 0, actionsAccepted = 0, actionsRejected = 0, actionsSuperseded = 0, actionTimeouts = 0, sendFailures = 0;
	std::uint64_t chatSent = 0, chatSampled = 0, chatSeen = 0, chatLost = 0, clientErrors = 0;
	std::uint64_t chatChecked = 0, chatMissing = 0;
	std::uint64_t fighterSent = 0, fighterRefused = 0, fighterUnseen = 0, roomClosed = 0;
	std::uint64_t probesOk = 0, probesFailed = 0, matchesStarted = 0, matchesOk = 0, matchesFailed = 0;
	Counts rejects, joinFailureReasons, lossReasons, matchFailureSteps;
	// This minute, cleared with each row; the all-run copies feed the summary.
	std::vector<std::uint64_t> joinMs, chatRtt, joinMsAll, chatRttAll, staleSamples;
	std::uint64_t staleMax = 0, staleCount = 0, staleSum = 0;
	// The deepest backlog any member of the room showed this minute.
	std::uint64_t queueMax = 0, queueBytesMax = 0, stagedMax = 0, helperLagMaxMs = 0, helperSilentMaxMs = 0;
};

} } }
