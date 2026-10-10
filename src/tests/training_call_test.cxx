// A player waiting in Training while in a room: when they are called out,
// the time they get to ready, and what gives the seat up.
#include "../session/TrainingCall.hxx"

#include <cstdio>

using namespace sf4e::room;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

using Step = TrainingCall::Step;

static Snapshot Room(MemberId local, MemberId p1, MemberId p2, TablePhase phase = TablePhase::Waiting) {
	Snapshot room;
	room.localMember = local;
	for (std::size_t i = 0; i < room.tables.size(); ++i) room.tables[i].id = static_cast<std::uint8_t>(i);
	room.tables[1].p1 = p1; room.tables[1].p2 = p2; room.tables[1].phase = phase;
	return room;
}
static TrainingCall::Input In(bool training, bool menu, bool canReady = false, bool autoAccept = false) {
	TrainingCall::Input in;
	in.inTraining = training; in.atMainMenu = menu; in.canReady = canReady; in.autoAccept = autoAccept;
	return in;
}

int main() {
	{
		// Who may go to Training from a room.
		CHECK(!TrainingCall::MayTrain(Snapshot{}));        // no member
		CHECK(TrainingCall::MayTrain(Room(2, 0, 0)));      // in the room, at no table
		auto queued = Room(3, 1, 2, TablePhase::Playing); queued.tables[1].queue = {3};
		CHECK(TrainingCall::MayTrain(queued));             // waiting for a seat while others play
		CHECK(TrainingCall::MayTrain(Room(2, 2, 0)));      // seated alone
		CHECK(!TrainingCall::MayTrain(Room(2, 1, 2)));     // someone is already waiting opposite
		auto readied = Room(2, 0, 2); readied.tables[1].ready[1] = true;
		CHECK(!TrainingCall::MayTrain(readied));
		for (const auto phase : {TablePhase::Ready, TablePhase::Playing, TablePhase::Paused, TablePhase::Closed})
			CHECK(!TrainingCall::MayTrain(Room(2, 0, 2, phase)));
	}
	{
		// Seated alone in Training: nothing, however long. A fighter sits down opposite: one call.
		TrainingCall call;
		auto room = Room(2, 0, 2);
		CHECK(call.Update(room, In(true, false), 1000) == Step::None);
		CHECK(call.Update(room, In(true, false), 900000) == Step::None);
		room.tables[1].p1 = 1;
		CHECK(call.Update(room, In(true, false), 901000) == Step::Call && call.Called());
		CHECK(call.Update(room, In(true, false), 901016) == Step::None);
		// Still leaving the battle: no window yet, so no time is taken from it.
		CHECK(call.Update(room, In(false, false), 902500) == Step::None && call.Remaining(902500) == 0);
		// At the main menu the window opens, once, with its full time.
		CHECK(call.Update(room, In(false, true), 903000) == Step::Open && !call.Called());
		CHECK(call.Remaining(903000) == TrainingCall::ReadyWindowMs);
		CHECK(call.Update(room, In(false, true, true), 910000) == Step::None && call.Remaining(910000) == 8000);
		// The player readies: the window is over and nothing is forfeited later.
		room.tables[1].ready[1] = true;
		CHECK(call.Update(room, In(false, true), 911000) == Step::None && call.Remaining(911000) == 0);
		CHECK(call.Update(room, In(false, true), 990000) == Step::None);
	}
	{
		// Not readied in time: the seat is given up, once.
		TrainingCall call;
		const auto room = Room(1, 1, 2);
		CHECK(call.Update(room, In(true, false), 0) == Step::Call);
		CHECK(call.Update(room, In(false, true), 2500) == Step::Open);
		CHECK(call.Update(room, In(false, true, true), 2500 + TrainingCall::ReadyWindowMs - 1) == Step::None);
		CHECK(call.Update(room, In(false, true, true), 2500 + TrainingCall::ReadyWindowMs) == Step::Forfeit);
		CHECK(call.Remaining(2500 + TrainingCall::ReadyWindowMs) == 0);
		// Still seated (the forfeit has not come back yet) and at the menu: no second call, no second forfeit.
		CHECK(call.Update(room, In(false, true, true), 40000) == Step::None);
		CHECK(call.Update(room, In(false, true, true), 400000) == Step::None);
	}
	{
		// Asked to be readied at once: one Ready as soon as the runtime allows it, and no forfeit after it lands.
		TrainingCall call;
		auto room = Room(1, 1, 2);
		CHECK(call.Update(room, In(true, false, false, true), 0) == Step::Call);
		CHECK(call.Update(room, In(false, true, false, true), 1000) == Step::Open);
		CHECK(call.Update(room, In(false, true, false, true), 1100) == Step::None);   // not possible yet
		CHECK(call.Update(room, In(false, true, true, true), 1200) == Step::Ready);
		CHECK(call.Update(room, In(false, true, true, true), 1300) == Step::None);    // once
		room.tables[1].ready[0] = true;
		CHECK(call.Update(room, In(false, true, true, true), 60000) == Step::None);
		// A Ready that never lands still runs into the window's end.
		TrainingCall stuck;
		const auto same = Room(1, 1, 2);
		CHECK(stuck.Update(same, In(true, false, false, true), 0) == Step::Call);
		CHECK(stuck.Update(same, In(false, true, true, true), 100) == Step::Open);
		CHECK(stuck.Update(same, In(false, true, true, true), 200) == Step::Ready);
		CHECK(stuck.Update(same, In(false, true, true, true), 100 + TrainingCall::ReadyWindowMs) == Step::Forfeit);
	}
	{
		// The opponent leaves during the call or the window: both end, and nothing is forfeited.
		TrainingCall call;
		auto room = Room(2, 1, 2);
		CHECK(call.Update(room, In(true, false), 0) == Step::Call);
		room.tables[1].p1 = 0;
		CHECK(call.Update(room, In(true, false), 500) == Step::None && !call.Called());
		// Another fighter sits down while the player is still in Training: called again.
		room.tables[1].p1 = 5;
		CHECK(call.Update(room, In(true, false), 1000) == Step::Call);
		CHECK(call.Update(room, In(false, true), 3000) == Step::Open);
		room.tables[1].p1 = 0;
		CHECK(call.Update(room, In(false, true), 4000) == Step::None && call.Remaining(4000) == 0);
		CHECK(call.Update(room, In(false, true), 90000) == Step::None);
		// A different opponent in the window is a new matchup: the old window is gone, and at the menu nobody is called.
		TrainingCall swapped;
		auto table = Room(2, 1, 2);
		CHECK(swapped.Update(table, In(true, false), 0) == Step::Call);
		CHECK(swapped.Update(table, In(false, true), 1000) == Step::Open);
		table.tables[1].p1 = 7;
		CHECK(swapped.Update(table, In(false, true), 2000) == Step::None && swapped.Remaining(2000) == 0);
		CHECK(swapped.Update(table, In(false, true), 99000) == Step::None);
	}
	{
		// The battle never leaves: the call is forgotten and the seat is kept.
		TrainingCall call;
		const auto room = Room(2, 1, 2);
		CHECK(call.Update(room, In(true, false), 0) == Step::Call);
		CHECK(call.Update(room, In(true, false), TrainingCall::ArrivalMs - 1) == Step::None && call.Called());
		CHECK(call.Update(room, In(false, false), TrainingCall::ArrivalMs) == Step::None && !call.Called());
		// Out of Training by then, so nothing more; back in Training later, the call comes again.
		CHECK(call.Update(room, In(false, false), 60000) == Step::None);
		CHECK(call.Update(room, In(true, false), 61000) == Step::Call);
	}
	{
		// Never called at the main menu or without Training: the room's own readying is untouched.
		TrainingCall call;
		const auto room = Room(2, 1, 2);
		for (std::uint64_t now = 0; now < 120000; now += 5000) CHECK(call.Update(room, In(false, true, true, true), now) == Step::None);
		// A table that is playing or ready calls nobody.
		for (const auto phase : {TablePhase::Ready, TablePhase::Playing, TablePhase::Paused, TablePhase::Closed})
			{ TrainingCall busy; CHECK(busy.Update(Room(2, 1, 2, phase), In(true, false), 0) == Step::None); }
		// A spectator or a queued member is not seated: no call.
		auto queued = Room(3, 1, 2); queued.tables[1].queue = {3};
		CHECK(call.Update(queued, In(true, false), 0) == Step::None);
	}
	{
		// The call's identity is the one account of who called and for which
		// battle: the room's epoch, the table, the opponent and the battle the
		// call was sent to, from the call until the window opens.
		TrainingCall call;
		auto room = Room(2, 0, 2); room.roomEpoch = 11;
		auto in = In(true, false); in.generation = 40;
		CHECK(!call.Identity().Live());
		room.tables[1].p1 = 1;
		CHECK(call.Update(room, in, 0) == Step::Call);
		const auto first = call.Identity();
		CHECK(first.Live() && first.roomEpoch == 11 && first.table == 1 && first.opponent == 1 && first.generation == 40);
		// A later battle number while called does not move the call to another battle.
		in.generation = 41;
		CHECK(call.Update(room, in, 16) == Step::None && call.Identity() == first);
		// The opponent leaves: no call stands, and nobody new is called.
		room.tables[1].p1 = 0;
		CHECK(call.Update(room, in, 32) == Step::None && !call.Identity().Live());
		// Another sits down: a new call, never the earlier one.
		room.tables[1].p1 = 5;
		CHECK(call.Update(room, in, 48) == Step::Call);
		const auto second = call.Identity();
		CHECK(second.Live() && second.opponent == 5 && second != first);
		// Replaced in one step: the call is the new opponent's at once.
		room.tables[1].p1 = 6;
		CHECK(call.Update(room, in, 64) == Step::Call && call.Identity().opponent == 6 && call.Identity() != second);
		// The room changes under the same seats: the call is the old room's and goes.
		room.roomEpoch = 12;
		CHECK(call.Update(room, In(false, false), 80) == Step::None && !call.Identity().Live());
		// Back at the main menu the window opens and no call stands any more.
		CHECK(call.Update(room, in, 96) == Step::Call && call.Identity().roomEpoch == 12);
		CHECK(call.Update(room, In(false, true), 112) == Step::Open && !call.Identity().Live());
	}
	if (failures) { std::printf("%d failures\n", failures); return 1; }
	std::printf("Training call rules passed.\n");
	return 0;
}
