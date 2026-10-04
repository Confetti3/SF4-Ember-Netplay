// The opponent-ready call-out: when it rings, and when it stays quiet.
#include "../session/ReadyChime.hxx"

#include <cstdio>

using namespace sf4e::room;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

static Snapshot Seated(MemberId local, MemberId p1, MemberId p2) {
	Snapshot room;
	room.localMember = local;
	for (std::size_t i = 0; i < room.tables.size(); ++i) room.tables[i].id = static_cast<std::uint8_t>(i);
	room.tables[1].p1 = p1;
	room.tables[1].p2 = p2;
	return room;
}

// members[0] is the local player and members[1] the one in the other seat
// of table 1 (seat 1's member when the local player is not seated).
static Snapshot WithFighters(Snapshot room, int localFighter, int opponentFighter) {
	Member local, other;
	room.tables[1].phase = TablePhase::Waiting;
	local.id = room.localMember; local.fighter = localFighter;
	other.id = room.tables[1].p1 == room.localMember ? room.tables[1].p2 : room.tables[1].p1; other.fighter = opponentFighter;
	room.members = {local, other};
	return room;
}

int main() {
	{
		// The opponent readies first: one call-out, not one per tick.
		ReadyChime chime;
		auto room = Seated(2, 1, 2);
		CHECK(!chime.Update(room, 1000));
		room.tables[1].ready[0] = true;
		CHECK(chime.Update(room, 1100));
		CHECK(!chime.Update(room, 1200));
		CHECK(!chime.Update(room, 9000));
		// Taken back and given again: again, but only after the quiet time.
		room.tables[1].ready[0] = false;
		CHECK(!chime.Update(room, 1300));
		room.tables[1].ready[0] = true;
		CHECK(!chime.Update(room, 1400));
		room.tables[1].ready[0] = false;
		CHECK(!chime.Update(room, 6000));
		room.tables[1].ready[0] = true;
		CHECK(chime.Update(room, 6100));
	}
	{
		// The local player readied first, or is the one readying: nothing to say.
		ReadyChime chime;
		auto room = Seated(1, 1, 2);
		room.tables[1].ready[0] = true;
		CHECK(!chime.Update(room, 1000));
		room.tables[1].ready[1] = true;
		CHECK(!chime.Update(room, 1100));
	}
	{
		// Not seated (queued, watching, or between tables) and empty seats stay quiet.
		ReadyChime chime;
		auto room = Seated(3, 1, 2);
		room.tables[1].ready[0] = room.tables[1].ready[1] = true;
		CHECK(!chime.Update(room, 1000));
		auto alone = Seated(1, 1, 0);
		CHECK(!chime.Update(alone, 1100));
		Snapshot none;
		CHECK(!chime.Update(none, 1200));
	}
	{
		// Sitting down across from someone already waiting rings once.
		ReadyChime chime;
		auto room = Seated(2, 1, 0);
		CHECK(!chime.Update(room, 1000));
		room.tables[1].p2 = 2;
		room.tables[1].ready[0] = true;
		CHECK(chime.Update(room, 1100));
		// A different opponent who is already ready (a rotation) rings again
		// once the quiet time has passed.
		room.tables[1].p1 = 5;
		CHECK(chime.Update(room, 5200));
	}
	{
		// The opponent shows a new fighter between games: said once, held
		// until the local player readies.
		OpponentFighterWatch watch;
		auto room = WithFighters(Seated(2, 1, 2), 0, 31);
		CHECK(!watch.Update(room) && watch.Pending() == -1);
		room.members[0].fighter = 31;
		CHECK(!watch.Update(room) && watch.Pending() == -1); // the local fighter is not the opponent's
		room.members[1].fighter = 3;
		CHECK(watch.Update(room) && watch.Pending() == 3);
		CHECK(!watch.Update(room) && watch.Pending() == 3);
		room.tables[1].ready[1] = true;
		CHECK(!watch.Update(room) && watch.Pending() == -1);
	}
	{
		// A first fighter, a new opponent, a game in progress and no seat say nothing.
		OpponentFighterWatch watch;
		auto room = WithFighters(Seated(2, 1, 2), 0, -1);
		CHECK(!watch.Update(room));
		room.members[1].fighter = 4;
		CHECK(!watch.Update(room) && watch.Pending() == -1);
		room.tables[1].p1 = 3; room.members[1].id = 3; room.members[1].fighter = 5;
		CHECK(!watch.Update(room) && watch.Pending() == -1);
		room.tables[1].phase = TablePhase::Playing;
		room.members[1].fighter = 6;
		CHECK(!watch.Update(room) && watch.Pending() == -1);
		auto apart = WithFighters(Seated(9, 1, 2), 0, 1);
		CHECK(!watch.Update(apart));
		apart.members[1].fighter = 2;
		CHECK(!watch.Update(apart) && watch.Pending() == -1);
	}
	{
		// The opponent leaving the seat clears what was pending.
		OpponentFighterWatch watch;
		auto room = WithFighters(Seated(2, 1, 2), 0, 1);
		watch.Update(room);
		room.members[1].fighter = 7;
		CHECK(watch.Update(room) && watch.Pending() == 7);
		room.tables[1].p1 = 0;
		CHECK(!watch.Update(room) && watch.Pending() == -1);
	}
	if (failures == 0) std::printf("ready chime: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
