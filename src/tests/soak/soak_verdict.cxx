#include "soak_verdict.hxx"
#include <iomanip>
#include <sstream>

namespace sf4e { namespace test { namespace soak {

std::string Num(double value, int digits) {
	std::ostringstream text;
	text << std::fixed << std::setprecision(digits) << value;
	return text.str();
}

// The criteria a run has to meet. A room that stayed up but dropped its actions,
// lagged its chat by seconds or lost its members is not a pass.
std::vector<Check> Evaluate(const Stats& st, const RoomTally& room, const Limits& limits, double hours) {
	std::vector<Check> checks;
	const auto add = [&](const std::string& name, double measured, double limit, bool ok, const char* unit = "") {
		checks.push_back({name, Num(measured) + unit, "<= " + Num(limit) + unit, ok});
	};
	const double average = room.rows ? room.fractionSum / room.rows : 0;
	checks.push_back({"room stayed up", room.lost ? "lost" : "up", "up", !room.lost});
	add("members connected on average", 100 * average, 50, average >= 0.5, "%");
	checks.back().limit = ">= 50.0%";
	const double timeoutPct = st.actionsSent ? 100.0 * st.actionTimeouts / st.actionsSent : 0;
	add("actions timed out", timeoutPct, limits.maxTimeoutPct, timeoutPct <= limits.maxTimeoutPct, "%");
	if (st.chatRttAll.size() >= 5) {
		const double p95 = Percentile(st.chatRttAll, 0.95);
		add("chat round trip p95", p95, limits.maxChatP95Ms, p95 <= limits.maxChatP95Ms, " ms");
	}
	if (st.chatSampled >= 5) {
		const double lostPct = 100.0 * st.chatLost / st.chatSampled;
		add("timed chat lines never seen", lostPct, limits.maxChatLostPct, lostPct <= limits.maxChatLostPct, "%");
	}
	if (st.chatChecked >= 20) {
		const double missingPct = 100.0 * st.chatMissing / st.chatChecked;
		add("chat lines missing from members' snapshots", missingPct, limits.maxChatMissingPct, missingPct <= limits.maxChatMissingPct, "%");
	}
	if (st.matchesStarted >= 1) {
		const double failedPct = 100.0 * st.matchesFailed / st.matchesStarted;
		add("matches that failed or never started", failedPct, limits.maxMatchFailPct, failedPct <= limits.maxMatchFailPct, "%");
	}
	const double drops = static_cast<double>(st.controlLosses + st.helperCrashes) / (room.members * (std::max)(hours, 0.25));
	add("members dropped per member-hour", drops, limits.maxDropsPerMemberHour, drops <= limits.maxDropsPerMemberHour);
	const double joinFailPct = st.joins + st.joinFailures ? 100.0 * st.joinFailures / (st.joins + st.joinFailures) : 0;
	add("join attempts that failed", joinFailPct, limits.maxJoinFailPct, joinFailPct <= limits.maxJoinFailPct, "%");
	const double saturated = room.rows ? 100.0 * room.saturatedRows / room.rows : 0;
	add("minutes the client loop was saturated", saturated, 20, saturated <= 20, "%");
	return checks;
}

} } }
