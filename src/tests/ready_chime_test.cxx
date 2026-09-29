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
	if (failures == 0) std::printf("ready chime: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
