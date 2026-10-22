// An Arcade room's seating for a member who sits down without naming a
// cabinet. Other rooms never reach this code.
#include "RoomModel.hxx"

namespace sf4e { namespace room {

namespace {
// Nobody is readying or playing at the table, and one of its two seats is open.
bool Alone(const Table& table) {
	return table.phase == TablePhase::Idle && (table.p1 == 0) != (table.p2 == 0);
}
}

// A table where a fighter waits alone for an opponent, else an empty one: the
// first of either, as no table comes before another. None while every table
// is full or between a game's start and its end.
Table* RoomAuthority::ArcadeTable() {
	Table* empty = nullptr;
	for (auto& table : snapshot_.tables) {
		if (Alone(table)) return &table;
		if (!empty && table.phase == TablePhase::Idle && !table.p1 && !table.p2) empty = &table;
	}
	return empty;
}

} }
