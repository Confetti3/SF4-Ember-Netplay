#pragma once

// The replays of a battle's games that the game does not save itself. A
// Versus battle saves one replay: VsBattle's task (0x63CA40) saves at the
// first match or draw result (0x63C410) and then only waits for the battle
// to end. Every game an in-place rematch plays after that in the same battle
// (sf4e__Game__Battle__System.cxx, GetGameMode) is Ember's to keep. Its
// recording lasts until the next game's first round records over it
// (0x5D65A0), so it is built at the restart, and handed over once the
// restart is final: the next game's fight confirmed for a fighter, at once
// for a spectator, who plays confirmed frames only. A battle that closes
// first hands over its last game. Independent of Dimps, so these rules are
// tested without the game.

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace sf4e { namespace replay {

class InPlaceReplays {
public:
	using Bytes = std::vector<std::uint8_t>;
	// The battle restarts in place after a game. The battle's first game is
	// the game's own; a later one is built now. Passing the restart again,
	// after a rollback, builds it again.
	template<class Build> void Restarting(const Build& build) {
		held_ = handedOver_ ? build() : Bytes();
	}
	// The restart is final: the replay of the game before it, empty for the
	// battle's first game.
	Bytes Continued() {
		++handedOver_;
		return Take();
	}
	// The battle closes after a game with a result: the replay of that game
	// when the game does not save it. One that restarted is the replay built
	// then, since the next game may have recorded over it.
	template<class Build> Bytes Closed(const Build& build) {
		if (held_) return Take();
		return handedOver_ ? build() : Bytes();
	}
	void Reset() { held_.reset(); handedOver_ = 0; }

private:
	Bytes Take() {
		Bytes replay = held_ ? std::move(*held_) : Bytes();
		held_.reset();
		return replay;
	}
	std::optional<Bytes> held_;
	unsigned handedOver_ = 0;
};

} }
