#pragma once

// Rollback-safe native match-result confirmation.  This header is
// deliberately independent of Dimps, GGPO, and the room transport so its
// state machine can be tested without launching the game.

#include <cstdint>
#include <array>
#include "ConfirmedCheckpoint.hxx"

namespace sf4e {
namespace native_result {

enum class Flow : std::uint8_t {
	Other = 0,
	MatchResult,
	DrawResult
};

enum class Result : std::uint8_t {
	None = 0,
	P1Win,
	P2Win,
	Draw
};

inline Result Decode(Flow flow, int winnerIndex) {
	if (flow == Flow::DrawResult) {
		return Result::Draw;
	}
	if (flow != Flow::MatchResult) {
		return Result::None;
	}
	if (winnerIndex == 0) {
		return Result::P1Win;
	}
	if (winnerIndex == 1) {
		return Result::P2Win;
	}
	return Result::None;
}

// Samples use GGPO SAVE-state identities, including corrected resimulation
// saves. A result is publishable only when every input it contains is confirmed.
// Keeping history avoids requiring a rollback-free stretch of outer frames.
class Timeline {
public:
	static constexpr int Capacity = 64;
	void Reset() { samples_ = {}; }
	void Capture(int stateFrame, Flow flow, int winnerIndex) {
		if (stateFrame <= 0) return;
		auto& sample = samples_[stateFrame % Capacity];
		sample.frame = stateFrame;
		sample.result = Decode(flow, winnerIndex);
	}
	void Rewind(int stateFrame) {
		if (stateFrame < 0) { Reset(); return; }
		for (auto& sample : samples_)
			if (sample.frame > stateFrame) sample = {};
	}
	Result Confirmed(int lastConfirmedInput) const {
		const Sample* latest = nullptr;
		for (const auto& sample : samples_) {
			if (sample.result != Result::None && statehash::IsConfirmedCheckpoint(sample.frame, lastConfirmedInput) &&
				(!latest || sample.frame > latest->frame)) latest = &sample;
		}
		return latest ? latest->result : Result::None;
	}
private:
	struct Sample { int frame = -1; Result result = Result::None; };
	std::array<Sample, Capacity> samples_{};
};

} // namespace native_result
} // namespace sf4e
