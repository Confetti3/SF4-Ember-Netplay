#pragma once
#include <cstdint>
#include "../common/MatchResult.hxx"

namespace sf4e { namespace room {

struct Snapshot;
// Continuous results score a numbered game without closing the transport;
// FinishContinuousMatch closes it only after both fighters finish the series.
struct ContinuousSeries {
	bool enabled = false;
	bool closed = false;
	std::uint64_t games = 0;
	std::uint32_t initialScore[2] = {0, 0};
	std::uint32_t wins[2] = {0, 0};
	MatchResult lastResult = MatchResult::Abort;
	std::uint8_t finishMask = 0;
};

// Only casual private tables may offer this experimental host rule.
bool ContinuousAllowed(const Snapshot& snapshot);

} }
