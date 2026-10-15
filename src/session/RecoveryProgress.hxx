#pragma once

// Which room recovery changes the log names: a refused commit once per
// distinct refusal, an accepted import still waiting for its rebind or
// activation after that, and recovery resuming only once a commit has applied.
// A wait with no failure before it is ordinary catch-up and is not named.
// Pure component, unit tested.

#include <cstdint>
#include <string>

namespace sf4e { namespace session {

class RecoveryProgress {
public:
	enum class Change { None, Failed, Waiting, Resumed };
	// `failed`: the head commit was refused. `error`: why recovery is failed or
	// waiting, empty while it applies. `applied`: the last commit applied.
	Change Observe(bool failed, const std::string& error, std::uint64_t applied) {
		if (failed) {
			if (stalled_ && error == error_) return Change::None;
			if (!stalled_) appliedAtFailure_ = applied;
			stalled_ = true;
			error_ = error;
			return Change::Failed;
		}
		if (!stalled_) return Change::None;
		if (applied > appliedAtFailure_) {
			stalled_ = false;
			error_.clear();
			return Change::Resumed;
		}
		if (error.empty() || error == error_) return Change::None;
		error_ = error;
		return Change::Waiting;
	}

private:
	bool stalled_ = false;
	std::string error_;
	std::uint64_t appliedAtFailure_ = 0;
};

} }
