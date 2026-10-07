#pragma once

// What both players held on every frame of a replay, read from the replay
// file itself ("#BRP", ReplaySlots.hxx), and what they chose before it.
// The layout, from the game's recorder (0x5D6000, 0x7831C0) and 115 archived
// replays that add up to their own length:
//
//   0x08   u16 1, u16 8: the layout read here. Replays of another kind
//          (3, 3; the native online service's) are laid out otherwise and
//          are refused.
//   0x18   the number of rounds played, 1 to 7
//   0x20   player 1, and 0x150 after it player 2: fighter, costume, color,
//          personal action (-1: none), win quote, Ultra and handicap, each
//          a dword, in the order of the game's own selection record
//   0x2C6  u16, the round timer; 0x2E0 the rounds setting (1, 3 or 5)
//   0x320  one 0x88-byte record a round, its state as the round starts:
//          +0x1C the rounds player 1 has won so far, +0x58 player 2's,
//          +0x7C the bytes of its stream
//   after the last round record, the streams, back to back
//
// A stream is 3-byte little-endian records, one a frame: 22 bits, player 1's
// inputs in the low 11 and player 2's above, or, with 0x400000 set, how many
// more frames the value before it lasted. A player's 11 bits are the pad
// word's low byte and its 0x400, 0x800 and 0x2000 moved down to 0x100, 0x200
// and 0x400.
//
// Who won the last round is not in the file. It follows from the rounds when
// only one player was a round short of the match; for a deciding round it is
// byte 49 of the slot record the game wrote (0 player 1, 1 player 2), which
// agreed with the rounds in all 62 archived matches that have both. A record
// Ember made up (ReplaySlots.hxx: ExportFromReplayAlone) has a title, which
// the game's own never has, and tells nothing.
//
// Everything works on bytes in memory; ReplayInputsTest covers it on a
// replay built at these offsets.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ReplaySlots.hxx"

namespace sf4e { namespace replayinputs {

// A player's 11 bits. Left and Right are the screen's, not the fighter's
// back and forward: the file does not say which way a fighter faces.
constexpr unsigned Up = 0x1, Down = 0x2, Left = 0x4, Right = 0x8, LP = 0x10, MP = 0x20, LK = 0x40, MK = 0x80, HP = 0x100, HK = 0x200, Select = 0x400;
constexpr unsigned Directions = Up | Down | Left | Right;
// In the order a player reads them.
constexpr unsigned Buttons[6] = {LP, MP, HP, LK, MK, HK};
constexpr const char* ButtonNames[6] = {"LP", "MP", "HP", "LK", "MK", "HK"};

// A stretch of frames with the same inputs for both players.
struct Run { std::uint32_t frames; std::uint16_t inputs[2]; };
// wins: the rounds each player has won as this one starts.
struct Round { std::vector<Run> runs; std::uint32_t frames = 0; int wins[2] = {0, 0}; };
struct Player { int fighter = -1, costume = 0, color = 0, personalAction = -1, ultra = 0, handicap = 0; };
// roundsToWin: what the match was played to. recordWinner: the winner the
// slot record names, 0 or 1, or -1 without a record the game wrote.
struct Match { Player players[2]; std::vector<Round> rounds; int roundsToWin = 2, timer = 99, recordWinner = -1; };

// The longest round kept: ten minutes, far past any timer.
constexpr std::uint32_t kMostFrames = 36000;

// Reads a replay file, or an .emberreplay that wraps one. False, with out
// as it was, for anything else, another layout, or sizes that do not add up.
inline bool Parse(const std::uint8_t* data, std::size_t size, Match& out) {
	int recordWinner = -1;
	if (size >= replayslots::kExportHeaderBytes && !std::memcmp(data, replayslots::kExportMagic, 8)) {
		const std::uint8_t* record = data + 8;
		if (!record[26] && record[49] <= 1) recordWinner = record[49];
		data += replayslots::kExportHeaderBytes; size -= replayslots::kExportHeaderBytes;
	}
	if (size < 0x320 || std::memcmp(data, "#BRP", 4) || data[8] != 1 || data[9] || data[10] != 8 || data[11]) return false;
	const std::uint32_t rounds = replayslots::ReadU32(data + 0x18);
	if (rounds < 1 || rounds > 7) return false;
	std::size_t at = 0x320 + 0x88 * rounds, total = at;
	if (size < at) return false;
	for (std::uint32_t round = 0; round < rounds; round++) {
		const std::uint32_t bytes = replayslots::ReadU32(data + 0x320 + 0x88 * round + 0x7C);
		if (bytes % 3 || bytes > size) return false;
		total += bytes;
	}
	if (total != size) return false;
	Match match;
	match.recordWinner = recordWinner;
	match.roundsToWin = static_cast<int>((replayslots::ReadU32(data + 0x2E0) + 1) / 2);
	match.timer = data[0x2C6] | (data[0x2C7] << 8);
	for (int side = 0; side < 2; side++) {
		const std::uint8_t* p = data + 0x20 + side * 0x150;
		const auto field = [&](int index) { return static_cast<int>(replayslots::ReadU32(p + index * 4)); };
		match.players[side] = {field(0), field(1), field(2), field(3), field(5), field(6)};
	}
	for (std::uint32_t round = 0; round < rounds; round++) {
		Round made;
		made.wins[0] = static_cast<int>(replayslots::ReadU32(data + 0x320 + 0x88 * round + 0x1C));
		made.wins[1] = static_cast<int>(replayslots::ReadU32(data + 0x320 + 0x88 * round + 0x58));
		const std::size_t end = at + replayslots::ReadU32(data + 0x320 + 0x88 * round + 0x7C);
		for (; at < end; at += 3) {
			const std::uint32_t record = data[at] | (data[at + 1] << 8) | (data[at + 2] << 16);
			const bool repeat = (record & 0x400000) != 0;
			const std::uint32_t frames = repeat ? record & 0x3FFFFF : 1;
			const Run run = {frames, {static_cast<std::uint16_t>(record & 0x7FF), static_cast<std::uint16_t>((record >> 11) & 0x7FF)}};
			made.frames += frames;
			if (made.frames > kMostFrames) return false;
			// A repeat lengthens the run before it; one with nothing before it repeats no input.
			if (repeat && !made.runs.empty()) made.runs.back().frames += frames;
			else if (repeat) made.runs.push_back({frames, {0, 0}});
			else if (!made.runs.empty() && made.runs.back().inputs[0] == run.inputs[0] && made.runs.back().inputs[1] == run.inputs[1]) made.runs.back().frames++;
			else made.runs.push_back(run);
		}
		match.rounds.push_back(std::move(made));
	}
	out = std::move(match);
	return true;
}

// The rounds each player won. False when the last round's winner is not
// known: a deciding round without the game's record, or round counts that
// do not fit the match.
inline bool Score(const Match& match, int score[2]) {
	if (match.rounds.empty()) return false;
	const int* wins = match.rounds.back().wins;
	const int last = match.roundsToWin - 1;
	if (last < 0 || wins[0] < 0 || wins[1] < 0 || wins[0] > last || wins[1] > last) return false;
	const int winner = wins[0] == last && wins[1] < last ? 0 : wins[1] == last && wins[0] < last ? 1 : wins[0] == last ? match.recordWinner : -1;
	if (winner < 0) return false;
	score[0] = wins[0] + (winner == 0); score[1] = wins[1] + (winner == 1);
	return true;
}

// One player over the whole match. presses: per button, in Buttons order,
// the times it went down. jumps: the times Up went down. crouched: frames
// with Down held. actions: button presses and changes to a new direction.
struct Stats {
	std::uint32_t frames = 0, presses[6] = {}, jumps = 0, crouched = 0, actions = 0;
	// Actions a minute of play, and the share of it spent holding Down, in percent.
	unsigned PerMinute() const { return frames ? static_cast<unsigned>(static_cast<std::uint64_t>(actions) * 3600 / frames) : 0; }
	unsigned CrouchedPercent() const { return frames ? static_cast<unsigned>(static_cast<std::uint64_t>(crouched) * 100 / frames) : 0; }
};
inline Stats Count(const Match& match, int side) {
	Stats stats;
	for (const Round& round : match.rounds) {
		unsigned before = 0;
		for (const Run& run : round.runs) {
			const unsigned now = run.inputs[side], down = now & ~before;
			for (int button = 0; button < 6; button++) if (down & Buttons[button]) { stats.presses[button]++; stats.actions++; }
			if (down & Up) stats.jumps++;
			if ((now & Directions) != (before & Directions) && (now & Directions)) stats.actions++;
			if (now & Down) stats.crouched += run.frames;
			stats.frames += run.frames;
			before = now;
		}
	}
	return stats;
}

// "2:41" for a number of frames at 60 a second, and "0:12.35" with hundredths.
inline std::string Clock(std::uint32_t frames, bool hundredths = false) {
	char text[24];
	if (hundredths) std::snprintf(text, sizeof text, "%u:%02u.%02u", frames / 3600, frames / 60 % 60, frames % 60 * 100 / 60);
	else std::snprintf(text, sizeof text, "%u:%02u", frames / 3600, frames / 60 % 60);
	return text;
}
// A player's inputs as fighting game notation: the direction as on a numpad
// seen on screen (4 left, 6 right, 5 none), then the buttons held.
inline std::string Notation(unsigned inputs) {
	const int x = (inputs & Right ? 1 : 0) - (inputs & Left ? 1 : 0), y = (inputs & Up ? 1 : 0) - (inputs & Down ? 1 : 0);
	std::string text(1, static_cast<char>('5' + x + 3 * y));
	for (int button = 0; button < 6; button++) if (inputs & Buttons[button]) { text += text.size() == 1 ? " " : "+"; text += ButtonNames[button]; }
	if (inputs & Select) text += text.size() == 1 ? " SEL" : "+SEL";
	return text;
}
// A round as text, one line for each change: the time it starts at and what
// each player holds from there on.
inline std::string Log(const Round& round) {
	std::string text;
	std::uint32_t at = 0;
	for (const Run& run : round.runs) {
		text += Clock(at, true) + "   P1 " + Notation(run.inputs[0]) + "   P2 " + Notation(run.inputs[1]) + "\n";
		at += run.frames;
	}
	return text;
}

} }
