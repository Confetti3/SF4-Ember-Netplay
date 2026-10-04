// Checks the soak's verdict arithmetic (Evaluate) on made-up rooms, and its
// Elapsed clock difference, without helpers or room hosts.
#include "soak_verdict.hxx"
#include "../test_support.hxx"
#include <iostream>

using namespace sf4e::test::soak;

namespace {

// A full room of 16 that met every limit over an hour.
Stats Clean() {
	Stats st;
	st.joins = 16; st.actionsSent = 1000; st.chatSampled = 40; st.chatChecked = 400; st.matchesStarted = 10;
	for (std::uint64_t ms = 1000; ms <= 2000; ms += 100) st.chatRttAll.push_back(ms);
	return st;
}
RoomTally Full() {
	RoomTally room;
	room.rows = 60; room.fractionSum = 60; room.members = 16;
	return room;
}
const Check* Find(const std::vector<Check>& checks, const std::string& name) {
	for (const auto& check : checks) if (check.name == name) return &check;
	return nullptr;
}
bool Passes(const std::vector<Check>& checks) {
	for (const auto& check : checks) if (!check.ok) return false;
	return true;
}
bool Ok(const std::vector<Check>& checks, const std::string& name) {
	const auto* check = Find(checks, name);
	CHECK(check != nullptr);
	return check->ok;
}

}

int main() {
	const Limits limits;
	const std::string drops = "members dropped per member-hour";

	// A clean room passes, with every check that has enough samples.
	auto checks = Evaluate(Clean(), Full(), limits, 1.0);
	CHECK(Passes(checks));
	CHECK(checks.size() == 10);

	// A lost room fails however good its numbers are.
	auto room = Full();
	room.lost = true;
	CHECK(!Ok(Evaluate(Clean(), room, limits, 1.0), "room stayed up"));

	// Half the members connected on average is the floor.
	room = Full();
	room.fractionSum = 30;
	CHECK(Ok(Evaluate(Clean(), room, limits, 1.0), "members connected on average"));
	room.fractionSum = 29;
	CHECK(!Ok(Evaluate(Clean(), room, limits, 1.0), "members connected on average"));

	// Timeouts: 2 percent of actions is the limit.
	auto st = Clean();
	st.actionTimeouts = 20;
	CHECK(Ok(Evaluate(st, Full(), limits, 1.0), "actions timed out"));
	st.actionTimeouts = 21;
	CHECK(!Ok(Evaluate(st, Full(), limits, 1.0), "actions timed out"));

	// Chat round trips are judged only with 5 samples or more.
	st = Clean();
	st.chatRttAll = {9000, 9000, 9000, 9000};
	CHECK(Find(Evaluate(st, Full(), limits, 1.0), "chat round trip p95") == nullptr);
	st.chatRttAll.push_back(9000);
	CHECK(!Ok(Evaluate(st, Full(), limits, 1.0), "chat round trip p95"));

	// Drops are control losses per member-hour.
	st = Clean();
	st.controlLosses = 4;
	CHECK(Ok(Evaluate(st, Full(), limits, 1.0), drops));
	st.controlLosses = 5;
	CHECK(!Ok(Evaluate(st, Full(), limits, 1.0), drops));
	// A short run counts as a quarter of an hour: one drop in 6 minutes is 0.25.
	st.controlLosses = 1;
	CHECK(Ok(Evaluate(st, Full(), limits, 0.1), drops));
	st.controlLosses = 2;
	CHECK(!Ok(Evaluate(st, Full(), limits, 0.1), drops));

	// Join failures: 20 percent of attempts.
	st = Clean();
	st.joinFailures = 4; // 4 of 20
	CHECK(Ok(Evaluate(st, Full(), limits, 1.0), "join attempts that failed"));
	st.joinFailures = 5;
	CHECK(!Ok(Evaluate(st, Full(), limits, 1.0), "join attempts that failed"));

	// A saturated client loop for more than a fifth of the minutes fails the room.
	room = Full();
	room.saturatedRows = 12;
	CHECK(Ok(Evaluate(Clean(), room, limits, 1.0), "minutes the client loop was saturated"));
	room.saturatedRows = 13;
	CHECK(!Ok(Evaluate(Clean(), room, limits, 1.0), "minutes the client loop was saturated"));

	// The limits are the caller's.
	Limits strict;
	strict.maxTimeoutPct = 0;
	st = Clean();
	st.actionTimeouts = 1;
	CHECK(!Ok(Evaluate(st, Full(), strict, 1.0), "actions timed out"));

	// An elapsed time never wraps when the stamp is later than now.
	CHECK(Elapsed(5000, 3000) == 2000);
	CHECK(Elapsed(3000, 5000) == 0);

	std::cout << "Soak verdict passed" << std::endl;
	return 0;
}
