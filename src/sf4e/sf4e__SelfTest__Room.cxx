#include "sf4e__SelfTest.hxx"

#include <fstream>
#include <memory>
#include <string>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "sf4e__NetplayFacade.hxx"

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

// Done: the outcome is written (Finish).
enum class Stage { Setup, Join, Opening, Seat, Ready, Playing, Ending, Leaving, Done };

struct Room : Steps<Stage> {
	std::wstring inviteFile;
} s_room;

bool Host() { return Requested() == Test::RoomHost; }
std::shared_ptr<const NetplayFacade::RuntimeSnapshot> Now() { return NetplayFacade::GetRuntimeSnapshotShared(); }

// Commands as the room's controls build them, tagged with the current
// generation so a stale one is dropped as a stale press would be.
RuntimeCommand Command(CommandKind kind) {
	const auto now = Now();
	RuntimeCommand request;
	request.command = { kind, now->session.generation, {} };
	request.preferences = now->preferences;
	request.displayName = now->preferences.displayName;
	return request;
}
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

// A waiting step sends again every few seconds: the runtime drops a command
// whose generation moved on, and the room refuses one with a stale revision.
bool Again(int seconds) { return s_room.waited % (seconds * kSecond) == 1; }

// Each side its own name, since a room refuses a second member of the same
// name; a fixed delay, so Ready does not wait for the connection check; one round.
netplay::PlayerPreferences RoomPreferences() {
	netplay::PlayerPreferences preferences = Now()->preferences;
	preferences.displayName = Host() ? "SelfTest host" : "SelfTest guest";
	preferences.autoInputDelay = false;
	preferences.inputDelay = 2;
	preferences.tableRules.roundCount = 1;
	return preferences;
}

std::string ReadInvitation() {
	std::ifstream in(s_room.inviteFile);
	std::string invitation;
	std::getline(in, invitation);
	return invitation;
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
		// A room needs a controller: the keyboard, when none was kept, asked
		// for as the controller page asks.
		if (!now->controllerReady) {
			if (Again(3)) { RuntimeCommand request; request.command.generation = now->session.generation; request.inputAction = input::Action::UseKeyboard; NetplayFacade::SubmitRuntimeCommand(std::move(request)); }
			if (run.Late(15)) run.Fail("controller", "none could be assigned");
			break;
		}
		if (!now->helperReady || !now->canOpenRoom) { if (run.Late(60)) run.Fail("room", "rooms never became available: " + now->helperError); break; }
		RuntimeCommand request = Command(Host() ? CommandKind::HostRoom : CommandKind::SavePreferences);
		request.preferences = RoomPreferences();
		request.displayName = request.preferences.displayName;
		NetplayFacade::SubmitRuntimeCommand(std::move(request));
		run.Enter(Host() ? Stage::Opening : Stage::Join);
		break;
	}
	case Stage::Join: {
		// The guest's settings first, then the invitation from the other PC.
		if (now->preferences.displayName != RoomPreferences().displayName) { if (run.Late(10)) run.Fail("room", "the guest's settings were not taken"); break; }
		const std::string invitation = ReadInvitation();
		if (invitation.empty()) { if (run.Late(180)) run.Fail("room", "no invitation arrived from the host"); break; }
		RuntimeCommand request = Command(CommandKind::JoinInvite);
		request.command.invitation = invitation;
		NetplayFacade::SubmitRuntimeCommand(std::move(request));
		run.Enter(Stage::Opening);
		break;
	}
	case Stage::Opening:
		if (now->session.room == netplay::RoomState::Joined && (!Host() || !now->invitation.empty())) {
			if (Host()) {
				std::ofstream out(run.inviteFile, std::ios::trunc);
				out << now->invitation << '\n';
				out.flush();
				if (!out) { run.Fail("room", "the invitation could not be written", Stage::Leaving); break; }
			}
			Say(true, "room", now->room.name);
			run.Enter(Stage::Seat);
		}
		else if (!now->session.error.empty()) run.Fail("room", now->session.error);
		else if (run.Late(60)) run.Fail("room", "never joined");
		break;
	case Stage::Seat:
		// The host takes Player 1's seat at the first table, the guest Player 2's.
		if (now->localSlot == seat) { Say(true, "seat", "Player " + std::to_string(seat + 1)); run.Enter(Stage::Ready); break; }
		if (Again(3)) { RuntimeCommand request = Action(room::ActionKind::Queue); request.roomAction.seat = static_cast<std::int8_t>(seat); NetplayFacade::SubmitRuntimeCommand(std::move(request)); }
		if (run.Late(30)) run.Fail("seat", now->helperError.empty() ? "never taken" : now->helperError, Stage::Leaving);
		break;
	case Stage::Ready: {
		// Ready as Ryu on the first stage once both seats are filled, until the
		// match is live: a Ready parked while the room settles is sent again.
		if (now->session.match == netplay::MatchState::Playing) { Say(true, "match live"); run.Enter(Stage::Playing); break; }
		const auto& table = now->room.tables[0];
		if (table.p1 && table.p2 && now->session.match == netplay::MatchState::None && !now->session.readyPending && Again(5)) {
			RuntimeCommand request = Command(CommandKind::Ready);
			request.character.charaID = 0; request.character.unc_edition = Dimps::Game::Battle::ED_USF4;
			NetplayFacade::SubmitRuntimeCommand(std::move(request));
		}
		if (run.Late(120)) run.Fail("match", now->readyFailure.empty() ? "never went live" : now->readyFailure, Stage::Leaving);
		break;
	}
	case Stage::Playing:
		// Five seconds of the game, then the host ends it, as End game does.
		if (run.Late(5)) run.Enter(Stage::Ending);
		break;
	case Stage::Ending:
		if (now->session.match != netplay::MatchState::Playing && now->session.match != netplay::MatchState::Preparing) { Say(true, "match end"); run.Enter(Stage::Leaving); }
		else if (Host() && Again(5)) NetplayFacade::SubmitRuntimeCommand(Action(room::ActionKind::AbortMatch));
		else if (run.Late(60)) run.Fail("match end", "the game never ended", Stage::Leaving);
		break;
	case Stage::Leaving:
		if (now->session.room == netplay::RoomState::Idle) { Say(true, "left the room"); run.Enter(Stage::Done); Finish(); break; }
		if (Again(3)) NetplayFacade::SubmitRuntimeCommand(Command(CommandKind::LeaveRoom));
		if (run.Late(30)) run.Fail("leave", "the room never let go");
		break;
	case Stage::Done:
		break;
	}
}

} }
