#pragma once

// What is known about the battle being loaded, for the one log line that says
// what a stalled load was loading. Plain values (no game types): the module
// that owns the load fills it from the typed sources the game thread has at
// that moment (sf4e__BattleLoad.cxx), and says so when there is none.

#include <cstdio>
#include <optional>
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

enum class LoadMode {
	Versus,
	Training,
	// The load is in a mode the event reader does not cover (Arcade and the
	// like), or its event was not there: the fighters and stage are not read.
	Unread,
};

struct BattleLoadFacts {
	LoadMode mode = LoadMode::Unread;
	// Ember put this battle's picks into the game's event; false when the
	// game's own select did.
	bool emberSetUp = false;
	LoadSide sides[2];
	// Empty when the load cannot say.
	std::optional<int> stage;
};

inline const char* LoadModeName(LoadMode mode) {
	switch (mode) {
	case LoadMode::Versus: return "versus";
	case LoadMode::Training: return "training";
	case LoadMode::Unread: return "unread";
	}
	return "unread";
}

inline std::string FormatBattleLoad(const BattleLoadFacts& facts) {
	const char* who = facts.emberSetUp ? "" : "native ";
	if (facts.mode == LoadMode::Unread) return std::string("mode=") + (facts.emberSetUp ? "unread" : "native select") + ", fighters and stage not read";
	char line[256];
	const auto& a = facts.sides[0];
	const auto& b = facts.sides[1];
	const std::string stage = facts.stage ? std::to_string(*facts.stage) : "not read";
	std::snprintf(line, sizeof(line),
		"mode=%s%s P1 fighter=%d costume=%d color=%d ultra=%d edition=%d, P2 fighter=%d costume=%d color=%d ultra=%d edition=%d, stage=%s",
		who, LoadModeName(facts.mode), a.fighter, a.costume, a.color, a.ultra, a.edition, b.fighter, b.costume, b.color, b.ultra, b.edition, stage.c_str());
	return line;
}

} // namespace loadwatch
} // namespace sf4e
