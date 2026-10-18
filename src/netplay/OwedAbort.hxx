#pragma once

#include <cstdint>

// A seated fighter's own abort of a tournament game whose connection was lost,
// owed to the bridge when the room never ends that game. The bridge hears of a
// game only from the room's end, and in a two-member room the other fighter's
// game is the room's only voter: when it is gone, the abort this fighter sent
// may sit queued to it, and nothing ends the game. The room's end decides the
// game whenever it comes, and so does a result this PC confirmed before the
// connection went; only a game the room has plainly not ended is reported as
// aborted by the fighter itself.
namespace sf4e { namespace netplay { namespace tournament {

class OwedAbort {
public:
	// How long the room's control may be lost, and how long the room may take
	// at all, before the abort is reported. The coordination leader's heartbeat
	// and an election take about 10 s, so a room that is still there ends the
	// game well within WaitMs.
	static constexpr std::uint64_t ControlLostMs = 5000, WaitMs = 15000;

	// Where the room and the match stand on the owed generation this tick.
	struct View {
		// The room committed an end for the generation, or its table left it.
		bool ended = false;
		// This PC captured a confirmed result for the generation.
		bool captured = false;
		// The room's control connection is healthy.
		bool healthy = false;
		// The tournament match is still being played here.
		bool playing = false;
	};
	enum class Step { Wait, Report, Drop };

	void Owe(std::uint64_t generation, std::uint64_t nowMs) {
		generation_ = generation;
		owedAtMs_ = nowMs;
		lostSinceMs_ = 0;
		controlLost_ = false;
	}
	std::uint64_t Generation() const { return generation_; }

	// Report the abort, drop it (the room or a confirmed result decides the
	// game), or wait. Report and Drop end the debt.
	Step Next(const View& view, std::uint64_t nowMs) {
		if (!generation_) return Step::Wait;
		if (view.ended || view.captured || !view.playing) { generation_ = 0; return Step::Drop; }
		if (view.healthy) controlLost_ = false;
		else if (!controlLost_) { controlLost_ = true; lostSinceMs_ = nowMs; }
		const bool lostLong = controlLost_ && nowMs - lostSinceMs_ >= ControlLostMs;
		if (!lostLong && nowMs - owedAtMs_ < WaitMs) return Step::Wait;
		generation_ = 0;
		return Step::Report;
	}

private:
	std::uint64_t generation_ = 0, owedAtMs_ = 0, lostSinceMs_ = 0;
	bool controlLost_ = false;
};

} } }
