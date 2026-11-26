#pragma once
// Replay takeover (docs/design/REPLAY_TAKEOVER.md): a player takes one fighter
// of a replay the game plays and plays on from that moment, while the other
// fighter follows the replay or stands in as a training dummy. A takeover
// lasts until its round is over; Retry goes back to the moment it began, and
// Back to the replay plays on from there as recorded. This is the model: its
// phases, what the other fighter does, and a round's recording frame by frame.
// The commands that drive it are the replay controls' (ReplayTransport.hxx:
// StepOf); sf4e__ReplayTakeover runs it in the game; ReplayTakeoverTest covers it.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "ReplayInputs.hxx"

namespace sf4e { namespace takeover {

// What the fighter the player did not take does: what the replay recorded,
// or a dummy that stands, blocks standing or blocks crouching.
enum class Other : std::uint8_t { FollowReplay, Stand, Block, LowBlock };
constexpr int OtherCount = 4;
inline Other NextOther(Other other) { return static_cast<Other>((static_cast<int>(other) + 1) % OtherCount); }

// Off: the replay plays as recorded. Playing: the player has a fighter.
// RoundOver: the round ended under a takeover and the battle is held there,
// waiting for Retry or Back to the replay.
enum class Phase : std::uint8_t { Off, Playing, RoundOver };
struct Plan {
	int side = 0;
	Other other = Other::FollowReplay;
};
// What a takeover command does (ReplayTransport.hxx: StepOf).
enum class Step : std::uint8_t { TakePlayer1, TakePlayer2, Retry, BackToReplay, CycleOther };

// One round's recorded inputs, frame by frame (ReplayInputs::ReadStream).
class Recording {
public:
	Recording() = default;
	explicit Recording(const replayinputs::Round& round) : runs_(round.runs) {
		std::uint32_t start = 0;
		for (const auto& run : runs_) { starts_.push_back(start); start += run.frames; }
		frames_ = start;
	}
	std::uint32_t Frames() const { return frames_; }
	// The recorded bits of `side` on `frame`; nothing past the round's end.
	std::uint16_t At(int side, std::uint32_t frame) const {
		if (frame >= frames_ || side < 0 || side > 1) return 0;
		const auto at = std::upper_bound(starts_.begin(), starts_.end(), frame) - starts_.begin() - 1;
		return runs_[static_cast<std::size_t>(at)].inputs[side];
	}
private:
	std::vector<replayinputs::Run> runs_;
	std::vector<std::uint32_t> starts_;
	std::uint32_t frames_ = 0;
};

// What one battle frame of a takeover knows besides the plan: both sides'
// recorded bits for the frame, and whether the other fighter stands to the
// right of the player's.
struct FrameFeed {
	std::uint16_t recorded[2] = {0, 0};
	bool otherOnRight = false;
};
// The pad word the battle reads for the fighter the player did not take. A
// dummy blocks by holding away from the player's fighter: Right while it
// stands to the right. Directions are the screen's, as the pad word's are.
inline unsigned OtherWord(const Plan& plan, const FrameFeed& feed) {
	const unsigned away = feed.otherOnRight ? replayinputs::Right : replayinputs::Left;
	switch (plan.other) {
	case Other::FollowReplay: return replayinputs::PadWord(feed.recorded[plan.side == 0 ? 1 : 0]);
	case Other::Stand: return 0;
	case Other::Block: return away;
	case Other::LowBlock: return away | replayinputs::Down;
	}
	return 0;
}

} }
