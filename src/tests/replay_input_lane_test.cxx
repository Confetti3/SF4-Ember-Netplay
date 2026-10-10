// The live input lanes (common/ReplayInputLane.hxx) on a replay file built
// at the offsets ReplayInputs.hxx reads, looked up by round and frame as the
// recorder reports them. The repository has no captured replay file, so the
// file is made here the way the game writes one.

#include "../common/ReplayInputLane.hxx"

#include <cstring>
#include <vector>

#include "test_support.hxx"

using namespace sf4e::replayinputs;
namespace lane = sf4e::replaylane;
using sf4e::replayslots::Bytes;
using sf4e::replayslots::WriteU32;

namespace {

void Record(Bytes& stream, std::uint32_t value) {
	for (int i = 0; i < 3; i++) stream.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::uint32_t Both(unsigned p1, unsigned p2) { return p1 | (p2 << 11); }
// `frames` frames of the same inputs: the record and its repeat.
void Hold(Bytes& stream, unsigned p1, unsigned p2, std::uint32_t frames) {
	Record(stream, Both(p1, p2));
	if (frames > 1) Record(stream, 0x400000 | (frames - 1));
}

Bytes Replay(const std::vector<Bytes>& streams) {
	Bytes replay(0x320 + 0x88 * streams.size(), 0);
	std::memcpy(replay.data(), "#BRP", 4);
	replay[8] = 1; replay[10] = 8;
	WriteU32(replay.data() + 0x18, static_cast<std::uint32_t>(streams.size()));
	for (std::size_t round = 0; round < streams.size(); round++) {
		WriteU32(replay.data() + 0x320 + 0x88 * round + 0x7C, static_cast<std::uint32_t>(streams[round].size()));
		replay.insert(replay.end(), streams[round].begin(), streams[round].end());
	}
	return replay;
}

lane::Lanes Built(const std::vector<Bytes>& streams) {
	const Bytes file = Replay(streams);
	Match match;
	CHECK(Parse(file.data(), file.size(), match));
	return lane::Build(match);
}

void TestRows() {
	Bytes first, second;
	// Round 1 (from frame 0): nothing for 10 frames; P1 crouches 5, then
	// crouches with LP for 3 (LP down on the first), then Down+LP+HP for 2 (HP
	// added), then HP alone 4. P2 walks right from frame 10, pressing Select
	// on frames 12 and 13, which the lane does not show.
	Hold(first, 0, 0, 10);
	Hold(first, Down, Right, 2); Hold(first, Down, Right | Select, 2); Hold(first, Down, Right, 1);
	Hold(first, Down | LP, Right, 3);
	Hold(first, Down | LP | HP, Right, 2);
	Hold(first, HP, Right, 4);
	// Round 2: P1 taps MK twice, a frame apart, then holds back-down.
	Hold(second, MK, 0, 1); Hold(second, 0, 0, 1); Hold(second, MK, 0, 1); Hold(second, Down | Left, 0, 57);
	const lane::Lanes lanes = Built({first, second});
	CHECK(lanes.rounds[0].size() == 2 && lanes.rounds[1].size() == 2);
	// Per player: P2's Select presses fold into one walk.
	CHECK(lanes.rounds[0][0].size() == 5 && lanes.rounds[1][0].size() == 2);
	CHECK(lanes.rounds[1][0][1].held == Right && lanes.rounds[1][0][1].frames == 14 && lanes.rounds[1][0][1].start == 10);

	lane::Row rows[lane::kRows];
	// The last frame of round 1 (cursor 24 played frame 23).
	std::uint32_t frame = 0;
	CHECK(lane::PlayedFrame(24, frame) && frame == 23);
	CHECK(lane::Rows(lanes, 0, 0, frame, rows) == 5);
	CHECK(rows[0].held == HP && rows[0].frames == 4 && rows[0].pressed == 0);
	CHECK(rows[1].held == (Down | LP | HP) && rows[1].frames == 2 && rows[1].pressed == HP);
	CHECK(rows[2].held == (Down | LP) && rows[2].frames == 3 && rows[2].pressed == LP);
	CHECK(rows[3].held == Down && rows[3].frames == 5 && rows[3].pressed == 0);
	CHECK(rows[4].held == 0 && rows[4].frames == 10);
	// Inside a run, the newest row counts the frames up to the one shown.
	CHECK(lane::Rows(lanes, 0, 0, 16, rows) == 3 && rows[0].held == (Down | LP) && rows[0].frames == 2 && rows[1].frames == 5);
	// A run boundary: the first frame of a run is one frame of it.
	CHECK(lane::Rows(lanes, 0, 0, 15, rows) == 3 && rows[0].frames == 1 && rows[0].pressed == LP);
	CHECK(lane::Rows(lanes, 0, 0, 14, rows) == 2 && rows[0].held == Down && rows[0].frames == 5);
	// Frame 0, and before it.
	CHECK(lane::Rows(lanes, 0, 0, 0, rows) == 1 && rows[0].held == 0 && rows[0].frames == 1);
	CHECK(!lane::PlayedFrame(0, frame));
	// P2 at the same frame.
	CHECK(lane::Rows(lanes, 1, 0, 23, rows) == 2 && rows[0].held == Right && rows[0].frames == 14 && rows[1].frames == 10);
	// The cursor past the round's end shows its last frame.
	CHECK(lane::Rows(lanes, 0, 0, 5000, rows) == 5 && rows[0].frames == 4);
	// Round edges: round 2 starts afresh, with MK pressed on each tap.
	CHECK(lane::Rows(lanes, 0, 1, 0, rows) == 1 && rows[0].held == MK && rows[0].pressed == MK);
	CHECK(lane::Rows(lanes, 0, 1, 3, rows) == 4);
	CHECK(rows[0].held == (Down | Left) && rows[0].frames == 1 && rows[0].pressed == 0);
	CHECK(rows[1].held == MK && rows[1].pressed == MK && rows[2].held == 0 && rows[3].pressed == MK);
	// A round the replay does not have, a side that is not one, no room.
	CHECK(lane::Rows(lanes, 0, 2, 0, rows) == 0 && lane::Rows(lanes, 0, -1, 0, rows) == 0);
	CHECK(lane::Rows(lanes, 2, 0, 0, rows) == 0 && lane::Rows(lanes, 0, 0, 0, rows, 0) == 0);
}

void TestCapAndHeldButtons() {
	Bytes stream;
	// 40 alternating presses of LK: more rows than the lane shows.
	for (int i = 0; i < 40; i++) Hold(stream, i % 2 ? LK : 0, 0, 3);
	// LK held into LK+MK: only MK is new.
	Hold(stream, LK, 0, 2); Hold(stream, LK | MK, 0, 2);
	const lane::Lanes lanes = Built({stream});
	lane::Row rows[lane::kRows];
	CHECK(lane::Rows(lanes, 0, 0, 200, rows) == lane::kRows);
	CHECK(rows[0].held == (LK | MK) && rows[0].pressed == MK && rows[1].held == LK && rows[1].pressed == LK);
	// Newest first, and the 14th is the oldest shown.
	CHECK(rows[2].held == 0 && rows[2].frames == 3 && rows[13].frames == 3);
	// Fewer rows when asked.
	CHECK(lane::Rows(lanes, 0, 0, 200, rows, 3) == 3);
}

void TestEmptyRound() {
	// A round with no records has no rows.
	const lane::Lanes lanes = Built({Bytes{}});
	lane::Row rows[lane::kRows];
	CHECK(lanes.rounds[0].size() == 1 && lane::Rows(lanes, 0, 0, 0, rows) == 0);
}

}

int main() {
	TestRows();
	TestCapAndHeldButtons();
	TestEmptyRound();
	return 0;
}
