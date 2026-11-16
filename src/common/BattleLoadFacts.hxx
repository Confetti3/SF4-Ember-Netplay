#pragma once

// What is known about the battle being loaded, for the one log line that says
// what a stalled load was loading. Plain values (no game types): the module
// that owns the load fills it from the typed sources the game thread has at
// that moment (sf4e__BattleLoad.cxx), and says so when there is none.

#include <cstdio>
#include <string>

namespace sf4e {
namespace loadwatch {

struct LoadSide {
	int fighter = 0;
	int costume = 0;
	int color = 0;
	int ultra = 0;
	int edition = 0;
};

enum class LoadSource {
	// The game's own select screens chose the fighters; Ember does not hold them.
	Native,
	// A Versus battle Ember set up: a room's match, or the developer jump.
	EmberVersus,
	// Training on the picks of Ember's Training select, or a dummy change.
	EmberTraining,
};

struct BattleLoadFacts {
	LoadSource source = LoadSource::Native;
	LoadSide sides[2];
	int stage = 0;
};

inline std::string FormatBattleLoad(const BattleLoadFacts& facts) {
	if (facts.source == LoadSource::Native) return "mode=native select, fighters and stage not read";
	char line[256];
	const auto& a = facts.sides[0];
	const auto& b = facts.sides[1];
	std::snprintf(line, sizeof(line),
		"mode=%s P1 fighter=%d costume=%d color=%d ultra=%d edition=%d, P2 fighter=%d costume=%d color=%d ultra=%d edition=%d, stage=%d",
		facts.source == LoadSource::EmberTraining ? "training" : "versus",
		a.fighter, a.costume, a.color, a.ultra, a.edition, b.fighter, b.costume, b.color, b.ultra, b.edition, facts.stage);
	return line;
}

} // namespace loadwatch
} // namespace sf4e
