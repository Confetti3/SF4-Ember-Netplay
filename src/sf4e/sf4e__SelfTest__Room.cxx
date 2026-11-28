#include "sf4e__SelfTest__Online.hxx"

#include <string>

#include "sf4e__SelfTest__Attack.hxx"

// The room test: two PCs play one game of a private room through the
// runtime's commands, the ones the room's controls send. room-host opens the
// room and writes its invitation to the file SF4E_SELFTEST_INVITE names; the
// runner copies it to the other PC, where room-guest joins with it. Each sits,
// readies as Ryu and plays until the match is live; the host then ends the
// game and both leave.
//
// The room game test (room-game-host, room-game-guest) is the same room played
// through: the host sets a Max ping before the seats are taken, its fighter
// attacks until the game ends by itself, and the host then leaves first. The
// guest has to be left in the room as its host, with a room it can still
// change, before it leaves too.
namespace sf4e { namespace selftest {
namespace {

using online::Generation;
using online::Now;

// Done: the outcome is written (Finish).
enum class Stage { Setup, Join, Opening, MaxPing, Seat, Ready, Playing, Ending, Recovery, Writable, Leaving, Done };

struct Room : Steps<Stage> {
	std::wstring inviteFile;
	// The room game test: reads of the host's fighter's input (RoomMatchInput),
	// and the room's epoch before the host left.
	unsigned frames = 0;
	std::uint64_t epochBefore = 0;
} s_room;

// The limits the room game test sets: the host's before the seats, the guest's once the room is its own.
constexpr std::uint16_t kHostMaxPing = 150, kGuestMaxPing = 100;

bool Game() { return Requested() == Test::RoomGameHost || Requested() == Test::RoomGameGuest; }
bool Host() { return Requested() == Test::RoomHost || Requested() == Test::RoomGameHost; }

commands::RoomAction Action(room::ActionKind kind) {
	commands::RoomAction request;
	request.generation = Generation();
	const auto& room = Now()->Room();
	request.roomAction.kind = kind; request.roomAction.table = 0;
	request.roomAction.roomEpoch = room.roomEpoch; request.roomAction.revision = room.revision;
	request.roomAction.tableRevision = room.tables[0].revision;
	// Ending a game names the game it ends, as the room's own controls do.
	if (kind == room::ActionKind::AbortMatch) request.roomAction.matchGeneration = room.tables[0].matchGeneration;
	return request;
}

void SetMaxPing(std::uint16_t ms) {
	auto request = Action(room::ActionKind::SetMaxPing);
	request.roomAction.maxPingMs = ms;
	NetplayFacade::SubmitRuntimeCommand(std::move(request));
}

// This side's own entry on the roster; null until the room lists it.
const room::Member* Self(const room::Snapshot& room) {
	for (const auto& member : room.members) if (member.id == room.localMember) return &member;
	return nullptr;
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
		auto preferences = online::TestPreferences(*now, Host());
		if (Host()) { commands::HostRoom host; host.generation = Generation(); host.preferences = std::move(preferences); NetplayFacade::SubmitRuntimeCommand(std::move(host)); }
		else NetplayFacade::SubmitRuntimeCommand(commands::SavePreferences{Generation(), std::move(preferences)});
		run.Enter(Host() ? Stage::Opening : Stage::Join);
		break;
	}
	case Stage::Join: {
		// The guest's settings first, then the invitation from the other PC.
		if (now->preferences.displayName != online::TestPreferences(*now, Host()).displayName) { if (run.Late(10)) run.Fail("room", "the guest's settings were not taken"); break; }
		const std::string invitation = online::ReadLine(run.inviteFile);
		if (invitation.empty()) { if (run.Late(180)) run.Fail("room", "no invitation arrived from the host"); break; }
		commands::JoinRoom request;
		request.generation = Generation(); request.invitation = invitation;
		NetplayFacade::SubmitRuntimeCommand(std::move(request));
		run.Enter(Stage::Opening);
		break;
	}
	case Stage::Opening:
		if (now->session.room == netplay::RoomState::Joined && (!Host() || !now->invitation.empty())) {
			if (Host() && !online::WriteLine(run.inviteFile, now->invitation)) { run.Fail("room", "the invitation could not be written", Stage::Leaving); break; }
			Say(true, "room", now->Room().name);
			run.Enter(Game() ? Stage::MaxPing : Stage::Seat);
		}
		else if (!now->session.error.empty()) run.Fail("room", now->session.error);
		else if (run.Late(60)) run.Fail("room", "never joined");
		break;
	case Stage::MaxPing: {
		// The host sets the limit once the guest is in, so both seats are asked for under it.
		const auto& room = now->Room();
		if (room.members.size() >= 2 && room.maxPingMs == kHostMaxPing) { Say(true, "max ping", std::to_string(kHostMaxPing) + " ms"); run.Enter(Stage::Seat); break; }
		if (Host() && room.members.size() >= 2 && run.Every(3)) SetMaxPing(kHostMaxPing);
		if (run.Late(180)) run.Fail("max ping", room.members.size() < 2 ? "the guest never joined" : "the room never took the limit", Stage::Leaving);
		break;
	}
	case Stage::Seat:
		// The host takes Player 1's seat at the first table, the guest Player 2's.
		if (now->localSlot == seat) { Say(true, "seat", "Player " + std::to_string(seat + 1)); run.Enter(Stage::Ready); break; }
		if (run.Every(3)) { auto request = Action(room::ActionKind::Queue); request.roomAction.seat = static_cast<std::int8_t>(seat); NetplayFacade::SubmitRuntimeCommand(std::move(request)); }
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
		// The room game test plays the game to its end (RoomMatchInput).
		if (Game()) {
			if (now->session.match == netplay::MatchState::PostMatch) { Say(true, "game played to its end"); run.epochBefore = now->Room().roomEpoch; run.Enter(Stage::Recovery); }
			else if (!netplay::Fighting(now->session.match)) run.Fail("game", "it ended without a result: " + now->session.error, Stage::Leaving);
			else if (run.Late(300)) run.Fail("game", "it never ended", Stage::Leaving);
			break;
		}
		// Five seconds of the game, then the host ends it, as End game does.
		if (run.Late(5)) run.Enter(Stage::Ending);
		break;
	case Stage::Ending:
		if (now->session.match != netplay::MatchState::Playing && now->session.match != netplay::MatchState::Preparing) { Say(true, "match end"); run.Enter(Stage::Leaving); }
		else if (Host() && run.Every(5)) NetplayFacade::SubmitRuntimeCommand(Action(room::ActionKind::AbortMatch));
		else if (run.Late(60)) run.Fail("match end", "the game never ended", Stage::Leaving);
		break;
	case Stage::Recovery: {
		// The host leaves first. The guest is left alone in the room, as its host.
		if (Host()) { run.Enter(Stage::Leaving); break; }
		const auto& room = now->Room();
		const room::Member* self = Self(room);
		if (now->session.room != netplay::RoomState::Joined) run.Fail("room kept after its host left", "the room was lost: " + now->session.error);
		else if (room.members.size() == 1 && self && self->host) {
			Say(true, "room kept after its host left", "epoch " + std::to_string(run.epochBefore) + " to " + std::to_string(room.roomEpoch));
			run.Enter(Stage::Writable);
		}
		else if (run.Late(120)) run.Fail("room kept after its host left", std::to_string(room.members.size()) + " members, this side " + (self && self->host ? "the host" : "not the host"), Stage::Leaving);
		break;
	}
	case Stage::Writable:
		// The room the guest was left with takes a host's change.
		if (now->Room().maxPingMs == kGuestMaxPing) { Say(true, "room changed by its new host", std::to_string(kGuestMaxPing) + " ms"); run.Enter(Stage::Leaving); break; }
		if (run.Every(3)) SetMaxPing(kGuestMaxPing);
		if (run.Late(60)) run.Fail("room changed by its new host", now->helperError.empty() ? "the limit was never taken" : now->helperError, Stage::Leaving);
		break;
	case Stage::Leaving:
		if (now->session.room == netplay::RoomState::Idle) { Say(true, "left the room"); run.Enter(Stage::Done); Finish(); break; }
		if (run.Every(3)) NetplayFacade::SubmitRuntimeCommand(commands::LeaveRoom{Generation(), commands::CloseCause::SelfTest});
		if (run.Late(30)) run.Fail("leave", "the room never let go");
		break;
	case Stage::Done:
		break;
	}
}

void RoomMatchInput(int side, unsigned& mapped, unsigned& raw) {
	// The guest's fighter stands still; the host's attacks while the game is played.
	const bool attacks = Host() && s_room.stage == Stage::Playing;
	mapped = raw = attacks ? AttackInput(s_room.frames++, side == 0) : 0u;
}

} }
