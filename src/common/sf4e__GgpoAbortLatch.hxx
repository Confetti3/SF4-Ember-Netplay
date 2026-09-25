#pragma once

// GGPO callback re-entrancy latch.
//
// ggpo_close_session deletes the backend, and the pinned fork keeps calling
// the advance-frame callback from Sync::AdjustSimulation after a callback
// returns. A session must therefore never be closed while one of its
// callbacks is on the stack. The host wraps every callback body in a Scope
// and asks Request() when it wants to abort: inside a callback the abort is
// latched and completed later by Take() from the outer tick; outside a
// callback Request() returns false and the host closes immediately.
//
// Pure component: no GGPO or game dependencies, unit tested.

#include <stdio.h>
#include <string.h>

namespace sf4e {
namespace gate {

struct AbortLatch {
	int depth = 0;
	bool pending = false;
	char reason[256] = {};

	struct Scope {
		AbortLatch& latch;
		explicit Scope(AbortLatch& l) : latch(l) { ++latch.depth; }
		~Scope() { --latch.depth; }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	};

	bool InCallback() const { return depth > 0; }

	// Returns true when the abort was latched (a callback is on the stack)
	// and the caller must not close the session now. The first reason of a
	// burst is kept; later ones are already explained by it.
	bool Request(const char* why) {
		if (depth <= 0) {
			return false;
		}
		if (!pending) {
			pending = true;
			snprintf(reason, sizeof(reason), "%s", why ? why : "");
		}
		return true;
	}

	// Hands out a latched abort once no callback is on the stack. Returns
	// false (and leaves `out` untouched) when nothing is pending or a
	// callback is still running.
	bool Take(char* out, size_t outSize) {
		if (!pending || depth > 0) {
			return false;
		}
		pending = false;
		if (out && outSize) {
			snprintf(out, outSize, "%s", reason);
		}
		reason[0] = '\0';
		return true;
	}

	void Reset() {
		pending = false;
		reason[0] = '\0';
	}
};

} // namespace gate
} // namespace sf4e
