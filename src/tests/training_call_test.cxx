// A player waiting in Training while in a room: when they are called out,
// the time they get to ready, and what gives the seat up.
#include "../session/TrainingCall.hxx"
#include "../training/TrainingSession.hxx"

#include <cstdio>
#include "test_support.hxx"

using namespace sf4e::room;

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
		EXPECT(!TrainingCall::MayTrain(Snapshot{}));        // no member
		EXPECT(TrainingCall::MayTrain(Room(2, 0, 0)));      // in the room, at no table
		auto queued = Room(3, 1, 2, TablePhase::Playing); queued.tables[1].queue = {3};
		EXPECT(TrainingCall::MayTrain(queued));             // waiting for a seat while others play
		EXPECT(TrainingCall::MayTrain(Room(2, 2, 0)));      // seated alone
		EXPECT(!TrainingCall::MayTrain(Room(2, 1, 2)));     // someone is already waiting opposite
		auto readied = Room(2, 0, 2); readied.tables[1].ready[1] = true;
		EXPECT(!TrainingCall::MayTrain(readied));
		for (const auto phase : {TablePhase::Ready, TablePhase::Playing, TablePhase::Paused, TablePhase::Closed})
			EXPECT(!TrainingCall::MayTrain(Room(2, 0, 2, phase)));
	}
	{
		// Seated alone in Training: nothing, however long. A fighter sits down opposite: one call.
		TrainingCall call;
		auto room = Room(2, 0, 2);
		EXPECT(call.Update(room, In(true, false), 1000) == Step::None);
		EXPECT(call.Update(room, In(true, false), 900000) == Step::None);
		room.tables[1].p1 = 1;
		EXPECT(call.Update(room, In(true, false), 901000) == Step::Call && call.Called());
		EXPECT(call.Update(room, In(true, false), 901016) == Step::None);
		// Still leaving the battle: no window yet, so no time is taken from it.
		EXPECT(call.Update(room, In(false, false), 902500) == Step::None && call.Remaining(902500) == 0);
		// At the main menu the window opens, once, with its full time.
		EXPECT(call.Update(room, In(false, true), 903000) == Step::Open && !call.Called());
		EXPECT(call.Remaining(903000) == TrainingCall::ReadyWindowMs);
		EXPECT(call.Update(room, In(false, true, true), 910000) == Step::None && call.Remaining(910000) == 8000);
		// The player readies: the window is over and nothing is forfeited later.
		room.tables[1].ready[1] = true;
		EXPECT(call.Update(room, In(false, true), 911000) == Step::None && call.Remaining(911000) == 0);
		EXPECT(call.Update(room, In(false, true), 990000) == Step::None);
	}
	{
		// Not readied in time: the seat is given up, once.
		TrainingCall call;
		const auto room = Room(1, 1, 2);
		EXPECT(call.Update(room, In(true, false), 0) == Step::Call);
		EXPECT(call.Update(room, In(false, true), 2500) == Step::Open);
		EXPECT(call.Update(room, In(false, true, true), 2500 + TrainingCall::ReadyWindowMs - 1) == Step::None);
		EXPECT(call.Update(room, In(false, true, true), 2500 + TrainingCall::ReadyWindowMs) == Step::Forfeit);
		EXPECT(call.Remaining(2500 + TrainingCall::ReadyWindowMs) == 0);
		// Still seated (the forfeit has not come back yet) and at the menu: no second call, no second forfeit.
		EXPECT(call.Update(room, In(false, true, true), 40000) == Step::None);
		EXPECT(call.Update(room, In(false, true, true), 400000) == Step::None);
	}
	{
		// Asked to be readied at once: one Ready as soon as the runtime allows it, and no forfeit after it lands.
		TrainingCall call;
		auto room = Room(1, 1, 2);
		EXPECT(call.Update(room, In(true, false, false, true), 0) == Step::Call);
		EXPECT(call.Update(room, In(false, true, false, true), 1000) == Step::Open);
		EXPECT(call.Update(room, In(false, true, false, true), 1100) == Step::None);   // not possible yet
		EXPECT(call.Update(room, In(false, true, true, true), 1200) == Step::Ready);
		EXPECT(call.Update(room, In(false, true, true, true), 1300) == Step::None);    // once
		room.tables[1].ready[0] = true;
		EXPECT(call.Update(room, In(false, true, true, true), 60000) == Step::None);
		// A Ready that never lands still runs into the window's end.
		TrainingCall stuck;
		const auto same = Room(1, 1, 2);
		EXPECT(stuck.Update(same, In(true, false, false, true), 0) == Step::Call);
		EXPECT(stuck.Update(same, In(false, true, true, true), 100) == Step::Open);
		EXPECT(stuck.Update(same, In(false, true, true, true), 200) == Step::Ready);
		EXPECT(stuck.Update(same, In(false, true, true, true), 100 + TrainingCall::ReadyWindowMs) == Step::Forfeit);
	}
	{
		// The opponent leaves during the call or the window: both end, and nothing is forfeited.
		TrainingCall call;
		auto room = Room(2, 1, 2);
		EXPECT(call.Update(room, In(true, false), 0) == Step::Call);
		room.tables[1].p1 = 0;
		EXPECT(call.Update(room, In(true, false), 500) == Step::None && !call.Called());
		// Another fighter sits down while the player is still in Training: called again.
		room.tables[1].p1 = 5;
		EXPECT(call.Update(room, In(true, false), 1000) == Step::Call);
		EXPECT(call.Update(room, In(false, true), 3000) == Step::Open);
		room.tables[1].p1 = 0;
		EXPECT(call.Update(room, In(false, true), 4000) == Step::None && call.Remaining(4000) == 0);
		EXPECT(call.Update(room, In(false, true), 90000) == Step::None);
		// A different opponent in the window is a new matchup: the old window is gone, and at the menu nobody is called.
		TrainingCall swapped;
		auto table = Room(2, 1, 2);
		EXPECT(swapped.Update(table, In(true, false), 0) == Step::Call);
		EXPECT(swapped.Update(table, In(false, true), 1000) == Step::Open);
		table.tables[1].p1 = 7;
		EXPECT(swapped.Update(table, In(false, true), 2000) == Step::None && swapped.Remaining(2000) == 0);
		EXPECT(swapped.Update(table, In(false, true), 99000) == Step::None);
	}
	{
		// Choosing a fighter in Ember's selector, or a Ready still on its way
		// to the room, costs no window time; the main menu without either does.
		TrainingCall call;
		const auto room = Room(2, 1, 2);
		EXPECT(call.Update(room, In(true, false), 0) == Step::Call);
		EXPECT(call.Update(room, In(false, true), 1000) == Step::Open);
		EXPECT(call.Update(room, In(false, true), 6000) == Step::None && call.Remaining(6000) == 10000);
		auto choosing = In(false, true); choosing.choosingFighter = true;
		EXPECT(call.Update(room, choosing, 26000) == Step::None && call.Remaining(26000) == 10000);
		EXPECT(call.Update(room, In(false, true), 30000) == Step::None && call.Remaining(30000) == 6000);
		auto sending = In(false, true); sending.readyInFlight = true;
		EXPECT(call.Update(room, sending, 40000) == Step::None && call.Remaining(40000) == 6000);
		EXPECT(call.Update(room, In(false, true), 45999) == Step::None);
		EXPECT(call.Update(room, In(false, true), 46000) == Step::Forfeit);
	}
	{
		// Away from the main menu, in the game's own screens, no time is spent either.
		TrainingCall call;
		const auto room = Room(2, 1, 2);
		EXPECT(call.Update(room, In(true, false), 0) == Step::Call);
		EXPECT(call.Update(room, In(false, true), 1000) == Step::Open);
		EXPECT(call.Update(room, In(false, false), 30000) == Step::None && call.Remaining(30000) == TrainingCall::ReadyWindowMs);
	}
	{
		// However it is held, the window ends WindowCapMs after the player was back.
		TrainingCall call;
		const auto room = Room(2, 1, 2);
		EXPECT(call.Update(room, In(true, false), 0) == Step::Call);
		EXPECT(call.Update(room, In(false, true), 1000) == Step::Open);
		auto choosing = In(false, true); choosing.choosingFighter = true;
		EXPECT(call.Update(room, choosing, 1000 + TrainingCall::WindowCapMs - 1) == Step::None &&
			call.Remaining(1000 + TrainingCall::WindowCapMs - 1) == 1);
		EXPECT(call.Update(room, choosing, 1000 + TrainingCall::WindowCapMs) == Step::Forfeit);
	}
	{
		// Back into a Training battle while the window is open: called again for
		// that battle, never forgotten, and the time left is kept for the next window.
		TrainingCall call;
		const auto room = Room(2, 1, 2);
		auto in = In(true, false); in.generation = 40;
		EXPECT(call.Update(room, in, 0) == Step::Call);
		EXPECT(call.Update(room, In(false, true), 1000) == Step::Open);
		EXPECT(call.Update(room, In(false, true), 6000) == Step::None);
		in.generation = 41;
		EXPECT(call.Update(room, in, 7000) == Step::Call && call.Called() && call.Identity().generation == 41);
		EXPECT(call.Update(room, in, 7000 + TrainingCall::ArrivalMs) == Step::None && call.Called());
		EXPECT(call.Update(room, In(false, true), 30000) == Step::Open && call.Remaining(30000) == 10000);
		// Staying in Training past the cap gives the seat up.
		EXPECT(call.Update(room, in, 31000) == Step::Call);
		EXPECT(call.Update(room, in, 1000 + TrainingCall::WindowCapMs - 1) == Step::None);
		EXPECT(call.Update(room, in, 1000 + TrainingCall::WindowCapMs) == Step::Forfeit && !call.Called());
		// Reset by the forfeit: a later call starts with the full time and its own cap.
		EXPECT(call.Update(room, in, 70000) == Step::Call);
		EXPECT(call.Update(room, In(false, true), 71000) == Step::Open && call.Remaining(71000) == TrainingCall::ReadyWindowMs);
		EXPECT(call.Update(room, In(false, false), 71000 + TrainingCall::WindowCapMs - 1) == Step::None);
		EXPECT(call.Update(room, In(false, false), 71000 + TrainingCall::WindowCapMs) == Step::Forfeit);
	}
	{
		// The cap comes before any other step on the tick it is reached: back at
		// the main menu then is no new window, and Training then is no new call.
		const auto room = Room(2, 1, 2);
		const auto openThenCalled = [&](TrainingCall& call) {
			EXPECT(call.Update(room, In(true, false), 0) == Step::Call);
			EXPECT(call.Update(room, In(false, true), 1000) == Step::Open);
			EXPECT(call.Update(room, In(true, false), 2000) == Step::Call);
		};
		TrainingCall menu; openThenCalled(menu);
		EXPECT(menu.Update(room, In(false, true), 1000 + TrainingCall::WindowCapMs) == Step::Forfeit && menu.Remaining(1000 + TrainingCall::WindowCapMs) == 0);
		TrainingCall training; openThenCalled(training);
		EXPECT(training.Update(room, In(false, true), 3000) == Step::Open);
		EXPECT(training.Update(room, In(true, false), 1000 + TrainingCall::WindowCapMs) == Step::Forfeit && !training.Called());
	}
	{
		// The battle never leaves: the call is forgotten and the seat is kept.
		TrainingCall call;
		const auto room = Room(2, 1, 2);
		EXPECT(call.Update(room, In(true, false), 0) == Step::Call);
		EXPECT(call.Update(room, In(true, false), TrainingCall::ArrivalMs - 1) == Step::None && call.Called());
		EXPECT(call.Update(room, In(false, false), TrainingCall::ArrivalMs) == Step::None && !call.Called());
		// Out of Training by then, so nothing more; back in Training later, the call comes again.
		EXPECT(call.Update(room, In(false, false), 60000) == Step::None);
		EXPECT(call.Update(room, In(true, false), 61000) == Step::Call);
	}
	{
		// Never called at the main menu or without Training: the room's own readying is untouched.
		TrainingCall call;
		const auto room = Room(2, 1, 2);
		for (std::uint64_t now = 0; now < 120000; now += 5000) EXPECT(call.Update(room, In(false, true, true, true), now) == Step::None);
		// A table that is playing or ready calls nobody.
		for (const auto phase : {TablePhase::Ready, TablePhase::Playing, TablePhase::Paused, TablePhase::Closed})
			{ TrainingCall busy; EXPECT(busy.Update(Room(2, 1, 2, phase), In(true, false), 0) == Step::None); }
		// A spectator or a queued member is not seated: no call.
		auto queued = Room(3, 1, 2); queued.tables[1].queue = {3};
		EXPECT(call.Update(queued, In(true, false), 0) == Step::None);
	}
	{
		// The call's identity is the one account of who called and for which
		// battle: the room's epoch, the table, the opponent and the battle the
		// call was sent to, from the call until the window opens.
		TrainingCall call;
		auto room = Room(2, 0, 2); room.roomEpoch = 11;
		auto in = In(true, false); in.generation = 40;
		EXPECT(!call.Identity().Live());
		room.tables[1].p1 = 1;
		EXPECT(call.Update(room, in, 0) == Step::Call);
		const auto first = call.Identity();
		EXPECT(first.Live() && first.roomEpoch == 11 && first.table == 1 && first.opponent == 1 && first.generation == 40);
		// A later battle number while called does not move the call to another battle.
		in.generation = 41;
		EXPECT(call.Update(room, in, 16) == Step::None && call.Identity() == first);
		// The opponent leaves: no call stands, and nobody new is called.
		room.tables[1].p1 = 0;
		EXPECT(call.Update(room, in, 32) == Step::None && !call.Identity().Live());
		// Another sits down: a new call, never the earlier one.
		room.tables[1].p1 = 5;
		EXPECT(call.Update(room, in, 48) == Step::Call);
		const auto second = call.Identity();
		EXPECT(second.Live() && second.opponent == 5 && second != first);
		// The same opponent getting up and sitting down again is a new call,
		// with its own serial and so its own countdown.
		room.tables[1].p1 = 0; call.Update(room, in, 50);
		room.tables[1].p1 = 5;
		EXPECT(call.Update(room, in, 52) == Step::Call && call.Identity().opponent == 5 && call.Identity().serial != second.serial && call.Identity() != second);
		// Replaced in one step: the call is the new opponent's at once.
		room.tables[1].p1 = 6;
		EXPECT(call.Update(room, in, 64) == Step::Call && call.Identity().opponent == 6 && call.Identity() != second);
		// The room changes under the same seats: the call is the old room's and goes.
		room.roomEpoch = 12;
		EXPECT(call.Update(room, In(false, false), 80) == Step::None && !call.Identity().Live());
		// Back at the main menu the window opens and no call stands any more.
		EXPECT(call.Update(room, in, 96) == Step::Call && call.Identity().roomEpoch == 12);
		EXPECT(call.Update(room, In(false, true), 112) == Step::Open && !call.Identity().Live());
	}
	{
		// The call's owner in the outer tick's order: the room's updates land
		// (SessionClient::Step), the owner reconciles the call with them, and
		// then the battle updates (its countdown follows the call, then
		// ticks). Call A stands with one frame left when a snapshot arrives in
		// which its opponent leaves, or is replaced, with or without a go now
		// pressed for A: the next battle update follows the room as it is now.
		using sf4e::training::CallControl;
		using sf4e::training::LeaveCountdown;
		const std::uint64_t battle = 40;
		const auto publish = [](const CallOwner::Result& result) {
			CallControl control;
			control.call = result.state.call.serial; control.generation = result.state.call.generation; control.hurry = result.state.hurry;
			return control;
		};
		enum class Change { Leaves, Replaced, ReplacedAfterGoNow };
		for (const auto change : {Change::Leaves, Change::Replaced, Change::ReplacedAfterGoNow}) {
			CallOwner owner; sf4e::input::GoNowGate gate; LeaveCountdown countdown;
			auto room = Room(2, 1, 2); room.roomEpoch = 7;
			auto in = In(true, false); in.generation = battle;
			std::uint64_t now = 0;
			auto published = publish(owner.Reconcile(room, in, now, gate));
			const auto a = published.call;
			EXPECT(a != 0 && owner.Identity().opponent == 1);
			for (int frame = 0; frame < 200 && countdown.Left() != 1; ++frame) {
				countdown.Follow(published, battle, 120); countdown.Tick(true);
				published = publish(owner.Reconcile(room, in, now += 16, gate));
			}
			EXPECT(countdown.Left() == 1 && countdown.Call() == a);
			if (change == Change::ReplacedAfterGoNow) EXPECT(gate.Press(sf4e::input::GoNowGate::Source::Keyboard, true));
			room.tables[1].p1 = change == Change::Leaves ? 0 : 3;
			room.revision++;
			published = publish(owner.Reconcile(room, in, now += 16, gate));
			const bool started = countdown.Follow(published, battle, 120);
			if (change == Change::Leaves) {
				EXPECT(!published.call && !started && countdown.Left() == 0);
				EXPECT(!countdown.Tick(true) && countdown.Left() == 0);
			} else {
				EXPECT(published.call != a && started && countdown.Left() == 120 && countdown.Call() == published.call);
				EXPECT(!published.hurry && !countdown.Tick(true) && countdown.Left() == 119);
				EXPECT(owner.Dropped() == (change == Change::ReplacedAfterGoNow ? 1u : 0u));
			}
		}
	}
	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Training call rules passed.\n");
	return 0;
}
