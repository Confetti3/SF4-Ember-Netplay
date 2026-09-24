#pragma once

// Hands one frame shift from the pacing tick to the frame limiter and the
// shift it applied back. Reset moves the generation on, so a limiter call
// that took its request before a session reset cannot report into the new
// session's pacing debt, whatever thread it runs on (ledger A-012).

#include <mutex>

namespace sf4e {
namespace pacing {

class FrameShiftMailbox {
public:
	struct Taken {
		int requestUs = 0;
		unsigned generation = 0;
	};

	void Request(int us) {
		std::lock_guard<std::mutex> lock(mutex_);
		requestUs_ = us;
	}
	// Called by the limiter before it waits.
	Taken Take() {
		std::lock_guard<std::mutex> lock(mutex_);
		Taken taken{requestUs_, generation_};
		requestUs_ = 0;
		return taken;
	}
	// Called by the limiter after it waited. Dropped when a reset happened
	// since the matching Take.
	void Complete(unsigned generation, int appliedUs) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (generation == generation_) appliedUs_ += appliedUs;
	}
	int TakeApplied() {
		std::lock_guard<std::mutex> lock(mutex_);
		const int applied = appliedUs_;
		appliedUs_ = 0;
		return applied;
	}
	void Reset() {
		std::lock_guard<std::mutex> lock(mutex_);
		requestUs_ = appliedUs_ = 0;
		++generation_;
	}

private:
	std::mutex mutex_;
	int requestUs_ = 0, appliedUs_ = 0;
	unsigned generation_ = 0;
};

} // namespace pacing
} // namespace sf4e
