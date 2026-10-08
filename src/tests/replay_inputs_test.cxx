// A replay's inputs and what is counted from them, on a replay built at the
// offsets ReplayInputs.hxx reads. With a file name, prints that replay
// instead: its players, its rounds and what each player pressed.

#include "../common/ReplayInputs.hxx"

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <utility>

#include "test_support.hxx"

using namespace sf4e::replayinputs;
using sf4e::replayslots::Bytes;
using sf4e::replayslots::WriteU32;

static void Record(Bytes& stream, std::uint32_t value) {
	for (int i = 0; i < 3; i++) stream.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
static std::uint32_t Both(unsigned p1, unsigned p2) { return p1 | (p2 << 11); }

static Bytes Replay(const std::vector<Bytes>& streams) {
	Bytes replay(0x320 + 0x88 * streams.size(), 0);
	std::memcpy(replay.data(), "#BRP", 4);
	replay[8] = 1; replay[10] = 8;
	WriteU32(replay.data() + 0x18, static_cast<std::uint32_t>(streams.size()));
	// Player 1: fighter 25, costume 2, color 9, personal action 3, Ultra 1. Player 2: fighter 1, no personal action.
	const std::uint32_t p1[] = {25, 2, 9, 3, 44, 1, 0}, p2[] = {1, 0, 0, 0xFFFFFFFF, 50, 0, 2};
	for (int i = 0; i < 7; i++) { WriteU32(replay.data() + 0x20 + i * 4, p1[i]); WriteU32(replay.data() + 0x170 + i * 4, p2[i]); }
	for (std::size_t round = 0; round < streams.size(); round++) {
		WriteU32(replay.data() + 0x320 + 0x88 * round + 0x7C, static_cast<std::uint32_t>(streams[round].size()));
		replay.insert(replay.end(), streams[round].begin(), streams[round].end());
	}
	return replay;
}

static void TestParseAndCount() {
	Bytes first, second;
	// Round 1: nothing for 10 frames, P1 crouches for 5, presses LP for 2 while crouching, then HP alone; P2 jumps.
	Record(first, Both(0, 0)); Record(first, 0x400000 | 9);
	Record(first, Both(Down, 0)); Record(first, 0x400000 | 4);
	Record(first, Both(Down | LP, 0)); Record(first, Both(Down | LP, 0));
	Record(first, Both(HP, Up | Right));
	// Round 2: P1 presses LP twice, a frame apart.
	Record(second, Both(LP, 0)); Record(second, Both(0, 0)); Record(second, Both(LP, 0)); Record(second, 0x400000 | 57);
	const Bytes replay = Replay({first, second});

	Match match;
	CHECK(Parse(replay.data(), replay.size(), match));
	CHECK(match.players[0].fighter == 25 && match.players[0].costume == 2 && match.players[0].color == 9 && match.players[0].personalAction == 3 && match.players[0].ultra == 1);
	CHECK(match.players[1].fighter == 1 && match.players[1].personalAction == -1 && match.players[1].handicap == 2);
	CHECK(match.rounds.size() == 2 && match.rounds[0].frames == 18 && match.rounds[1].frames == 60);
	// Equal records in a row and their repeats are one run.
	CHECK(match.rounds[0].runs.size() == 4 && match.rounds[0].runs[0].frames == 10 && match.rounds[0].runs[2].frames == 2);
	CHECK(match.rounds[0].runs[3].inputs[0] == HP && match.rounds[0].runs[3].inputs[1] == (Up | Right));

	const Stats p1 = Count(match, 0), p2 = Count(match, 1);
	CHECK(p1.frames == 78 && p1.presses[0] == 3 && p1.presses[2] == 1 && p1.jumps == 0 && p1.crouched == 7);
	CHECK(p1.actions == 5 && p1.CrouchedPercent() == 8); // four presses and the crouch
	CHECK(p2.jumps == 1 && p2.actions == 1 && p2.presses[0] == 0);
	CHECK(p1.PerMinute() == 5 * 3600 / 78);

	CHECK(Notation(0) == "5" && Notation(Down | Right | LP | MK) == "3 LP+MK" && Notation(Up | Left) == "7" && Notation(Select) == "5 SEL");
	CHECK(Clock(161 * 60) == "2:41" && Clock(12 * 60 + 21, true) == "0:12.35");
	CHECK(Log(match.rounds[0]) == "0:00.00   P1 5   P2 5\n0:00.16   P1 2   P2 5\n0:00.25   P1 2 LP   P2 5\n0:00.28   P1 5 HP   P2 9\n");

	// Wrapped as an .emberreplay it reads the same.
	Bytes wrapped(sf4e::replayslots::kExportMagic, sf4e::replayslots::kExportMagic + 8);
	wrapped.resize(sf4e::replayslots::kExportHeaderBytes, 0);
	wrapped.insert(wrapped.end(), replay.begin(), replay.end());
	Match again;
	CHECK(Parse(wrapped.data(), wrapped.size(), again) && again.rounds.size() == 2 && again.rounds[1].frames == 60);
}

// The score: from the rounds when one player was a round short, from the
// game's record for a deciding round, and unknown without that record.
static void TestScore() {
	Bytes stream;
	Record(stream, Both(LP, 0));
	const auto played = [&](std::initializer_list<std::pair<int, int>> starts, int setting) {
		Bytes replay = Replay(std::vector<Bytes>(starts.size(), stream));
		WriteU32(replay.data() + 0x2E0, setting);
		std::size_t round = 0;
		for (const auto& start : starts) { WriteU32(replay.data() + 0x320 + 0x88 * round + 0x1C, start.first); WriteU32(replay.data() + 0x320 + 0x88 * round + 0x58, start.second); round++; }
		return replay;
	};
	const auto wrapped = [](const Bytes& replay, int winner, bool made) {
		Bytes file(sf4e::replayslots::kExportMagic, sf4e::replayslots::kExportMagic + 8);
		file.resize(sf4e::replayslots::kExportHeaderBytes, 0);
		file[8 + 49] = static_cast<std::uint8_t>(winner);
		if (made) file[8 + 26] = '2';
		file.insert(file.end(), replay.begin(), replay.end());
		return file;
	};
	Match match;
	int score[2] = {-1, -1};
	Bytes replay = played({{0, 0}, {0, 1}}, 3); // player 2 took both
	CHECK(Parse(replay.data(), replay.size(), match) && match.roundsToWin == 2 && Score(match, score) && score[0] == 0 && score[1] == 2);
	replay = played({{0, 0}, {1, 0}, {1, 1}}, 3); // a deciding round
	CHECK(Parse(replay.data(), replay.size(), match) && !Score(match, score));
	Bytes file = wrapped(replay, 1, false);
	CHECK(Parse(file.data(), file.size(), match) && Score(match, score) && score[0] == 1 && score[1] == 2);
	file = wrapped(replay, 0, false);
	CHECK(Parse(file.data(), file.size(), match) && Score(match, score) && score[0] == 2 && score[1] == 1);
	file = wrapped(replay, 0, true); // a record Ember made up
	CHECK(Parse(file.data(), file.size(), match) && !Score(match, score));
	replay = played({{0, 0}, {1, 0}, {2, 0}}, 5); // first to three
	CHECK(Parse(replay.data(), replay.size(), match) && Score(match, score) && score[0] == 3 && score[1] == 0);
	replay = played({{0, 0}, {3, 0}}, 3); // more rounds won than the match has
	CHECK(Parse(replay.data(), replay.size(), match) && !Score(match, score));
}

static void TestRefusals() {
	Bytes stream;
	Record(stream, Both(LP, 0));
	const Bytes replay = Replay({stream});
	Match match;
	match.rounds.resize(5);
	Bytes bad = replay;
	bad[8] = 3; bad[10] = 3; // the other layout
	CHECK(!Parse(bad.data(), bad.size(), match));
	bad = replay; bad.push_back(0); // a byte the rounds do not account for
	CHECK(!Parse(bad.data(), bad.size(), match));
	bad = replay; WriteU32(bad.data() + 0x18, 9);
	CHECK(!Parse(bad.data(), bad.size(), match));
	bad = replay; bad[0] = 'X';
	CHECK(!Parse(bad.data(), bad.size(), match));
	CHECK(!Parse(replay.data(), 0x100, match));
	// A repeat count past any round.
	Bytes endless;
	Record(endless, Both(LP, 0)); Record(endless, 0x400000 | 0x3FFFFF);
	bad = Replay({endless});
	CHECK(!Parse(bad.data(), bad.size(), match));
	CHECK(match.rounds.size() == 5); // untouched by every refusal
}

int main(int argc, char** argv) {
	if (argc > 1) {
		std::ifstream file(argv[1], std::ios::binary);
		const Bytes bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		Match match;
		if (!Parse(bytes.data(), bytes.size(), match)) { printf("not a replay this reads\n"); return 1; }
		for (int side = 0; side < 2; side++) {
			const Stats stats = Count(match, side);
			printf("P%d fighter %d costume %d color %d ultra %d: %u a minute, %u jumps, crouched %u%%,", side + 1, match.players[side].fighter,
				match.players[side].costume, match.players[side].color, match.players[side].ultra, stats.PerMinute(), stats.jumps, stats.CrouchedPercent());
			for (int button = 0; button < 6; button++) printf(" %s %u", ButtonNames[button], stats.presses[button]);
			printf("\n");
		}
		int score[2] = {-1, -1};
		if (Score(match, score)) printf("score %d-%d\n", score[0], score[1]); else printf("score unknown\n");
		for (std::size_t round = 0; round < match.rounds.size(); round++)
			printf("round %u: %s, %u changes\n", static_cast<unsigned>(round + 1), Clock(match.rounds[round].frames).c_str(), static_cast<unsigned>(match.rounds[round].runs.size()));
		if (argc > 2) printf("%s", Log(match.rounds[0]).c_str());
		return 0;
	}
	TestParseAndCount();
	TestScore();
	TestRefusals();
	printf("replay_inputs_test: all tests passed\n");
	return 0;
}
