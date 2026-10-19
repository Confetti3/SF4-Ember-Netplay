#include "sf4e__SelfTest__Online.hxx"

#include <string>

// The room test: two PCs play one game of a private room through the
// runtime's commands, the ones the room's controls send. room-host opens the
// room and writes its invitation to the file SF4E_SELFTEST_INVITE names; the
// runner copies it to the other PC, where room-guest joins with it. Each sits,
// readies as Ryu and plays until the match is live; the host then ends the
// game and both leave.
namespace sf4e { namespace selftest {
namespace {

using NetplayFacade::RuntimeCommand;
using netplay::CommandKind;
using online::Command;
using online::Now;

// Done: the outcome is written (Finish).
enum class Stage { Setup, Join, Opening, Seat, Ready, Playing, Ending, Leaving, Done };

struct Room : Steps<Stage> {
	std::wstring inviteFile;
} s_room;

bool Host() { return Requested() == Test::RoomHost; }

RuntimeCommand Action(room::ActionKind kind) {
	RuntimeCommand request = Command(CommandKind::RoomAction);
	const auto& room = Now()->room;
	request.roomAction.kind = kind; request.roomAction.table = 0;
	request.roomAction.roomEpoch = room.roomEpoch; request.roomAction.revision = room.revision;
	request.roomAction.tableRevision = room.tables[0].revision;
	// Ending a game names the game it ends, as the room's own controls do.
	if (kind == room::ActionKind::AbortMatch) request.roomAction.matchGeneration = room.tables[0].matchGeneration;
	return request;
}

}

bool StartRoom() {
	s_room.inviteFile = Environment(L"SF4E_SELFTEST_INVITE");
	if (s_room.inviteFile.empty()) { Say(false, "room", "SF4E_SELFTEST_INVITE is not set"); return false; }
	s_room.Enter(Stage::Setup);
	return true;
}

// The host opens the room and the guest joins it; the rest is the same on both sides.
void TickRoom() {
	Room& run = s_room;
	run.waited++;
	const auto now = Now();
	const int seat = Host() ? 0 : 1;
	switch (run.stage) {
	case Stage::Setup: {
		if (!online::SetupReady(run, *now)) break;
		RuntimeCommand request = Command(Host() ? CommandKind::HostRoom : CommandKind::SavePreferences);
		request.preferences = online::TestPreferences(*now, Host());
		request.displayName = request.preferences.displayName;
		NetplayFacade::SubmitRuntimeCommand(std::move(request));
		run.Enter(Host() ? Stage::Opening : Stage::Join);
		break;
	}
	case Stage::Join: {
		// The guest's settings first, then the invitation from the other PC.
		if (now->preferences.displayName != online::TestPreferences(*now, Host()).displayName) { if (run.Late(10)) run.Fail("room", "the guest's settings were not taken"); break; }
		const std::string invitation = online::ReadLine(run.inviteFile);
		if (invitation.empty()) { if (run.Late(180)) run.Fail("room", "no invitation arrived from the host"); break; }
		RuntimeCommand request = Command(CommandKind::JoinInvite);
		request.command.invitation = invitation;
		NetplayFacade::SubmitRuntimeCommand(std::move(request));
		run.Enter(Stage::Opening);
		break;
	}
	case Stage::Opening:
		if (now->session.room == netplay::RoomState::Joined && (!Host() || !now->invitation.empty())) {
			if (Host() && !online::WriteLine(run.inviteFile, now->invitation)) { run.Fail("room", "the invitation could not be written", Stage::Leaving); break; }
			Say(true, "room", now->room.name);
			run.Enter(Stage::Seat);
		}
		else if (!now->session.error.empty()) run.Fail("room", now->session.error);
		else if (run.Late(60)) run.Fail("room", "never joined");
		break;
	case Stage::Seat:
		// The host takes Player 1's seat at the first table, the guest Player 2's.
		if (now->localSlot == seat) { Say(true, "seat", "Player " + std::to_string(seat + 1)); run.Enter(Stage::Ready); break; }
		if (run.Every(3)) { RuntimeCommand request = Action(room::ActionKind::Queue); request.roomAction.seat = static_cast<std::int8_t>(seat); NetplayFacade::SubmitRuntimeCommand(std::move(request)); }
		if (run.Late(30)) run.Fail("seat", now->helperError.empty() ? "never taken" : now->helperError, Stage::Leaving);
		break;
	case Stage::Ready: {
		// Ready as Ryu on the first stage once both seats are filled, until the
		// match is live: a Ready parked while the room settles is sent again.
		if (now->session.match == netplay::MatchState::Playing) { Say(true, "match live"); run.Enter(Stage::Playing); break; }
		if (run.Every(5)) online::ReadyAsRyu(*now, 0);
		if (run.Late(120)) run.Fail("match", now->readyFailure.empty() ? "never went live" : now->readyFailure, Stage::Leaving);
		break;
	}
	case Stage::Playing:
		// Five seconds of the game, then the host ends it, as End game does.
		if (run.Late(5)) run.Enter(Stage::Ending);
		break;
	case Stage::Ending:
		if (now->session.match != netplay::MatchState::Playing && now->session.match != netplay::MatchState::Preparing) { Say(true, "match end"); run.Enter(Stage::Leaving); }
		else if (Host() && run.Every(5)) NetplayFacade::SubmitRuntimeCommand(Action(room::ActionKind::AbortMatch));
		else if (run.Late(60)) run.Fail("match end", "the game never ended", Stage::Leaving);
		break;
	case Stage::Leaving:
		if (now->session.room == netplay::RoomState::Idle) { Say(true, "left the room"); run.Enter(Stage::Done); Finish(); break; }
		if (run.Every(3)) NetplayFacade::SubmitRuntimeCommand(Command(CommandKind::LeaveRoom));
		if (run.Late(30)) run.Fail("leave", "the room never let go");
		break;
	case Stage::Done:
		break;
	}
}

} }
