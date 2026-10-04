#pragma once

// The criteria a run has to meet, as a pure function of a room's counters, so
// it can be checked without helpers or room hosts (soak_verdict_test.cxx).
#include "soak_stats.hxx"
#include <cstddef>

namespace sf4e { namespace test { namespace soak {

// The run fails when a room goes past any of these. Each has an option.
struct Limits {
	double maxTimeoutPct = 2, maxChatP95Ms = 5000, maxChatLostPct = 5, maxChatMissingPct = 1, maxMatchFailPct = 25, maxDropsPerMemberHour = 0.25, maxJoinFailPct = 20;
};

// What a room's minute rows added up to, besides its Stats.
struct RoomTally {
	bool lost = false;
	unsigned rows = 0, saturatedRows = 0;
	double fractionSum = 0; // the fraction of members connected, summed over the rows
	std::size_t members = 0;
};

struct Check { std::string name, measured, limit; bool ok; };
std::string Num(double value, int digits = 1);

// The checks for one room over a run of `hours`; the room fails when any is not ok.
std::vector<Check> Evaluate(const Stats& st, const RoomTally& room, const Limits& limits, double hours);

} } }
