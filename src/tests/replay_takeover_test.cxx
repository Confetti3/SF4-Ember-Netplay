// Replay takeover's model (common/ReplayTakeover.hxx): a round's recording
// read from the recorder's stream form, frame by frame, and what the battle
// reads for the fighter the player did not take. The commands, their steps and
// the pad's gestures are the replay controls' (ReplayTransportTest).

#include "../common/ReplayTakeover.hxx"

#include <cstdint>
#include <vector>

#include "test_support.hxx"

using namespace sf4e::takeover;
namespace replayinputs = sf4e::replayinputs;

namespace {

void TestSteps() {
	// A step acts only in a phase it still applies to: two takes in one batch take one fighter, and nothing retries without a takeover.
	CHECK(Applies(Step::TakePlayer1, Phase::Off) && !Applies(Step::TakePlayer2, Phase::Playing) && !Applies(Step::TakePlayer1, Phase::RoundOver));
	CHECK(!Applies(Step::Retry, Phase::Off) && Applies(Step::Retry, Phase::Playing) && Applies(Step::Retry, Phase::RoundOver));
	CHECK(!Applies(Step::BackToReplay, Phase::Off) && Applies(Step::BackToReplay, Phase::RoundOver));
	for (Phase phase : {Phase::Off, Phase::Playing, Phase::RoundOver}) CHECK(Applies(Step::CycleOther, phase));
}

void TestOthers() {
	// The other fighter's modes go round.
	Other other = Other::FollowReplay;
	const Other order[] = {Other::Stand, Other::Block, Other::LowBlock, Other::FollowReplay};
	for (Other next : order) { other = NextOther(other); CHECK(other == next); }
}

// One 3-byte record: Player 1's 11 bits, then Player 2's, or a repeat of the run before.
void Record(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
	bytes.push_back(static_cast<std::uint8_t>(value)); bytes.push_back(static_cast<std::uint8_t>(value >> 8)); bytes.push_back(static_cast<std::uint8_t>(value >> 16));
}
std::uint32_t Inputs(std::uint16_t p1, std::uint16_t p2) { return p1 | (static_cast<std::uint32_t>(p2) << 11); }

void TestRecording() {
	std::vector<std::uint8_t> bytes;
	Record(bytes, Inputs(replayinputs::Right, 0x10));
	Record(bytes, 0x400000 | 4);  // four more frames of it
	Record(bytes, Inputs(replayinputs::Left | 0x400, 0));
	replayinputs::Round round;
	std::uint32_t played = 0;
	CHECK(replayinputs::ReadStream(bytes.data(), bytes.size(), played, round));
	CHECK(played == 6 && round.frames == 6);
	const Recording recording(round);
	CHECK(recording.Frames() == 6);
	for (std::uint32_t frame = 0; frame < 5; ++frame) CHECK(recording.At(0, frame) == replayinputs::Right && recording.At(1, frame) == 0x10);
	CHECK(recording.At(0, 5) == (replayinputs::Left | 0x400) && recording.At(1, 5) == 0);
	// Select, recorded at 0x400, is the pad word's 0x2000.
	CHECK(replayinputs::PadWord(recording.At(0, 5)) == (replayinputs::Left | 0x2000u));
	// Past the round's end, and for no side, nothing.
	CHECK(recording.At(0, 6) == 0 && recording.At(1, 1000) == 0);
	CHECK(recording.At(2, 0) == 0 && recording.At(-1, 0) == 0);
	// An empty round is all nothing.
	CHECK(Recording().At(0, 0) == 0 && Recording().Frames() == 0);
	// A match longer than the reader takes is refused.
	std::vector<std::uint8_t> endless;
	Record(endless, Inputs(0, 0));
	Record(endless, 0x400000 | 0x3FFFFF);
	replayinputs::Round refused;
	std::uint32_t most = replayinputs::kMostFrames - 2;
	CHECK(!replayinputs::ReadStream(endless.data(), endless.size(), most, refused));
}

void TestOtherWord() {
	FrameFeed feed;
	feed.recorded[0] = 0x101; feed.recorded[1] = 0x202;
	Plan plan;
	// Followed: the other fighter reads its recorded word, whichever side was taken.
	plan.side = 0;
	CHECK(OtherWord(plan, feed) == replayinputs::PadWord(0x202));
	plan.side = 1;
	CHECK(OtherWord(plan, feed) == replayinputs::PadWord(0x101));
	// The dummy: still, or holding away from the player's fighter, low or not.
	plan.side = 0; plan.other = Other::Stand;
	CHECK(OtherWord(plan, feed) == 0);
	plan.other = Other::Block;
	feed.otherOnRight = true;
	CHECK(OtherWord(plan, feed) == replayinputs::Right);
	feed.otherOnRight = false;
	CHECK(OtherWord(plan, feed) == replayinputs::Left);
	plan.other = Other::LowBlock;
	CHECK(OtherWord(plan, feed) == (replayinputs::Left | replayinputs::Down));
	feed.otherOnRight = true;
	CHECK(OtherWord(plan, feed) == (replayinputs::Right | replayinputs::Down));
}

}

int main() {
	TestSteps();
	TestOthers();
	TestRecording();
	TestOtherWord();
	return 0;
}
