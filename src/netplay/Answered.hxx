#pragma once

#include <cstdint>
#include <string>

namespace sf4e { namespace netplay {

// One kind of request's last answer: whether one is in flight, the helper's
// code when the last failed, how many have finished, answered or not, and how
// many values were installed. A failed request moves `finished` but not
// `revision`, so a reader can tell a newer value from a newer failure. Both
// counts carry on across a change of service or Ember ID (RankingRequests.hxx,
// LevelRequests.hxx).
template <class Value>
struct Answered {
	Value value;
	bool loaded = false;
	bool loading = false;
	std::string error;
	std::uint64_t finished = 0, revision = 0;
	// The runtime's clock, milliseconds, when the value arrived.
	std::uint64_t installedMs = 0;
};

} }
