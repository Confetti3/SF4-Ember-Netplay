#pragma once

// The live input lanes over a replay: for each player, what they held on the
// frames up to the one the game just played, newest first, as runs of the
// same input, the shape of Training's input history. Built once from the
// replay file Ember plays (ReplayInputs.hxx) and indexed by the round and
// the frame the recorder reports, so a lane pauses, steps and slows with the
// replay. Directions are the screen's, as the file has them.
//
// ReplayInputLaneTest covers it on replays built at the file's offsets.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ReplayInputs.hxx"

namespace sf4e { namespace replaylane {

// The rows a lane shows.
constexpr int kRows = 14;
// The directions and the six fight buttons; Select is left out.
constexpr unsigned kShown = replayinputs::Directions | replayinputs::LP | replayinputs::MP | replayinputs::HP |
	replayinputs::LK | replayinputs::MK | replayinputs::HK;
constexpr unsigned kButtons = kShown & ~replayinputs::Directions;

// One row: what was held, the buttons that went down on its first frame,
// and how many frames it lasted up to the frame shown.
struct Row { std::uint16_t held = 0, pressed = 0; std::uint32_t frames = 0; };

// One player's stretch of a round with the same shown input.
struct Run { std::uint32_t start = 0, frames = 0; std::uint16_t held = 0; };
// Per player, per round.
struct Lanes { std::vector<std::vector<Run>> rounds[2]; };

inline Lanes Build(const replayinputs::Match& match) {
	Lanes lanes;
	for (int side = 0; side < 2; side++) {
		lanes.rounds[side].reserve(match.rounds.size());
		for (const auto& round : match.rounds) {
			std::vector<Run> runs;
			std::uint32_t start = 0;
			for (const auto& run : round.runs) {
				const std::uint16_t held = static_cast<std::uint16_t>(run.inputs[side] & kShown);
				if (!runs.empty() && runs.back().held == held) runs.back().frames += run.frames;
				else { Run made; made.start = start; made.frames = run.frames; made.held = held; runs.push_back(made); }
				start += run.frames;
			}
			lanes.rounds[side].push_back(std::move(runs));
		}
	}
	return lanes;
}

// The frame the recorder just played: its cursor names the next one. False
// before the round's first frame.
inline bool PlayedFrame(std::uint32_t cursor, std::uint32_t& frame) {
	if (!cursor) return false;
	frame = cursor - 1;
	return true;
}

// Up to `most` rows of a player's lane ending at `frame` of `round`, newest
// first; the count. A frame past the round's end shows its last frame; a
// round the replay does not have shows nothing.
inline int Rows(const Lanes& lanes, int side, int round, std::uint32_t frame, Row* out, int most = kRows) {
	if (side < 0 || side > 1 || round < 0 || static_cast<std::size_t>(round) >= lanes.rounds[side].size() || most <= 0) return 0;
	const std::vector<Run>& runs = lanes.rounds[side][static_cast<std::size_t>(round)];
	if (runs.empty()) return 0;
	const std::uint32_t last = runs.back().start + runs.back().frames - 1;
	if (frame > last) frame = last;
	// The run holding the frame: the last that starts at or before it.
	auto at = std::upper_bound(runs.begin(), runs.end(), frame, [](std::uint32_t f, const Run& run) { return f < run.start; });
	std::size_t index = static_cast<std::size_t>(at - runs.begin()) - 1;
	int count = 0;
	for (;;) {
		const Run& run = runs[index];
		Row row;
		row.held = run.held;
		row.pressed = static_cast<std::uint16_t>(run.held & kButtons & ~(index ? runs[index - 1].held : 0u));
		row.frames = count ? run.frames : frame - run.start + 1;
		out[count++] = row;
		if (count == most || !index) break;
		--index;
	}
	return count;
}

} }
