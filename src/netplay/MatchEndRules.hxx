#pragma once

// Pure rules for the end of a room match, kept game-free so they can be unit
// tested. NetplayRuntime applies them on the game thread.

#include "../session/RoomModel.hxx"

#include <cstdint>

namespace sf4e { namespace netplay {

// The room delivers one committed match end to a client more than once: the
// live event, the terminal receipt replayed from a checkpoint, and resends
// until the client acknowledges it. Each copy is processed (the handlers are
// idempotent), but only the first is worth a log line.
class MatchEndLog {
public:
	bool First(std::uint8_t table, std::uint64_t generation) {
		if (seen_ && table == table_ && generation == generation_) return false;
		seen_ = true; table_ = table; generation_ = generation;
		return true;
	}
private:
	bool seen_ = false;
	std::uint8_t table_ = 0;
	std::uint64_t generation_ = 0;
};

// Positive evidence, from the local projection of one table, that a match
// generation has ended: the table has started a newer generation, or it still
// shows this one outside play (a start sets the generation and Playing
// together). A projection that has not reached this generation yet proves
// nothing; the native grant can arrive before it. The projection can lag the
// authority but never lead it, so an ended generation has ended there too, and
// a generation-scoped Unwatch or AbortMatch for it would only be rejected with
// WrongGeneration.
inline bool GenerationEnded(const room::Table& table, std::uint64_t generation) {
	if (table.matchGeneration != generation) return table.matchGeneration > generation;
	return table.phase != room::TablePhase::Playing && table.phase != room::TablePhase::Paused;
}

} }
