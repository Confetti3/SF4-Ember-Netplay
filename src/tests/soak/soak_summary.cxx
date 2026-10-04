#include "soak_summary.hxx"
#include <iomanip>
#include <iostream>

namespace sf4e { namespace test { namespace soak {

void PrintSummary(const std::vector<std::unique_ptr<Room>>& rooms, Clock ranMs, bool& failed) {
	std::cout << "\n==== Soak summary after " << Hms(ranMs) << " ====\n";
	Stats total;
	const double hours = ranMs / 3600000.0;
	for (const auto& roomPtr : rooms) {
		const auto& room = *roomPtr;
		const auto& st = room.st;
		const double average = room.rows ? room.fractionSum / room.rows : 0;
		RoomTally tally;
		tally.lost = room.lost; tally.rows = room.rows; tally.saturatedRows = room.saturatedRows;
		tally.fractionSum = room.fractionSum; tally.members = room.members.size();
		const auto checks = Evaluate(st, tally, options.limits, hours);
		const bool bad = std::any_of(checks.begin(), checks.end(), [](const Check& check) { return !check.ok; });
		failed = failed || bad;
		std::cout << "room " << room.number << (bad ? "  FAILED" : "  ok") << ": avg members connected " << std::fixed << std::setprecision(1) << average * room.members.size()
			<< "/" << room.members.size() << ", joins " << st.joins << " (rejoins " << st.rejoins << "), join failures " << st.joinFailures << ", refused " << st.refused
			<< ", control losses " << st.controlLosses << " (helper crashes " << st.helperCrashes << "), degraded " << st.degraded << "\n"
			<< "    actions " << st.actionsSent << " sent, " << st.actionsAccepted << " accepted, " << st.actionsRejected << " rejected, " << st.actionTimeouts << " timed out"
			<< "; rejects: " << (st.rejects.empty() ? "none" : Flat(st.rejects)) << "\n"
			<< "    chat " << st.chatSent << " sent, " << st.chatSampled << " timed, " << st.chatSeen << " seen, " << st.chatLost << " lost; rtt p50 " << Percentile(st.chatRttAll, 0.5)
			<< " ms p95 " << Percentile(st.chatRttAll, 0.95) << " ms max " << Maximum(st.chatRttAll) << " ms\n"
			<< "    join ms p50 " << Percentile(st.joinMsAll, 0.5) << " p95 " << Percentile(st.joinMsAll, 0.95) << " max " << Maximum(st.joinMsAll)
			<< "; checks " << st.probesOk << " ok " << st.probesFailed << " failed; matches " << st.matchesOk << "/" << st.matchesStarted << " ok"
			<< (st.matchFailureSteps.empty() ? "" : " (failed at " + Flat(st.matchFailureSteps) + ")") << "\n";
		if (!st.joinFailureReasons.empty()) std::cout << "    join failures by reason: " << Flat(st.joinFailureReasons) << "\n";
		if (!st.lossReasons.empty()) std::cout << "    control losses by reason: " << Flat(st.lossReasons) << "\n";
		for (const auto& check : checks) if (!check.ok) std::cout << "    FAILED criterion: " << check.name << " " << check.measured << " (limit " << check.limit << ")\n";
		total.joins += st.joins; total.joinFailures += st.joinFailures; total.controlLosses += st.controlLosses; total.helperCrashes += st.helperCrashes;
	}
	std::cout << "total: " << total.joins << " joins, " << total.joinFailures << " join failures, " << total.controlLosses << " control losses ("
		<< total.helperCrashes << " of them helper crashes)\n" << (failed ? "RESULT: FAILED (see the failed criteria above)"
		: "RESULT: PASSED (every room met every criterion)") << std::endl;
}

} } }
