#pragma once

// How a spectator keeps up with the host's GGPO stream. Pure and game-free so
// it can be unit tested; fSystem::BattleUpdate applies it.
//
// P1 sends each confirmed frame as soon as the fighters confirm it and never
// waits for a spectator. A spectator that runs slower, or hitches while it
// keeps receiving, falls behind for good unless it plays faster for a while.
// Before this, the lag only grew: once it passed GGPO's 64-frame spectator
// ring an unplayed frame was overwritten and synchronize_input returned
// GENERAL_FAILURE, which ended the view (field logs, v0.9.9).
//
// The spectator keeps ReserveFrames in hand against arrival jitter and plays
// up to MaxExtraFrames more frames in one engine update while it holds more.

namespace sf4e {

struct SpectatorCatchUp {
	static constexpr int ReserveFrames = 4;
	static constexpr int MaxExtraFrames = 3;

	// backlogFrames: frames received and not yet played, counted after the
	// frame this update already played. Never more than the backlog.
	static int ExtraFrames(int backlogFrames) {
		const int excess = backlogFrames - ReserveFrames;
		if (excess <= 0) return 0;
		const int extra = excess <= 15 ? 1 : excess <= 60 ? 2 : MaxExtraFrames;
		return extra < excess ? extra : excess;
	}
};

// Per log window: the largest backlog seen and the frames played to catch up.
struct SpectatorBacklogWindow {
	int maxBacklog = 0;
	unsigned catchUpFrames = 0;

	void Sample(int backlogFrames) { if (backlogFrames > maxBacklog) maxBacklog = backlogFrames; }
	void CaughtUp(int frames) { if (frames > 0) catchUpFrames += static_cast<unsigned>(frames); }
};

}
