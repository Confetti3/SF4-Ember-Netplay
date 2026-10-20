#include "sf4e__SelfTest__Online.hxx"

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

// The room layout test: on one PC, in a private room it hosts, the room
// board's column settings are saved the way the board saves them
// (RuntimeCommand::roomSettings), since a room locks the player's other
// settings. The runtime has to take them while the room is open, and
// settings.json, which the next start reads, has to hold them. The old values
// go back the same way, and the test leaves.
namespace sf4e { namespace selftest {
namespace {

using NetplayFacade::RuntimeCommand;
using netplay::CommandKind;
using online::Command;
using online::Now;

enum class Stage { Setup, Opening, Save, Written, Restore, Leaving, Done };

struct RoomLayout : Steps<Stage> {
	netplay::PlayerPreferences before;
} s_layout;

// The column the test saves: none of it the defaults.
netplay::PlayerPreferences Chosen(const netplay::PlayerPreferences& now) {
	auto chosen = now;
	chosen.roomSideWidth = 42; chosen.roomMemberShare = 70;
	chosen.roomMembersHidden = true; chosen.roomChatHidden = false;
	return chosen;
}
bool SameColumn(const netplay::PlayerPreferences& a, const netplay::PlayerPreferences& b) {
	return a.roomSideWidth == b.roomSideWidth && a.roomMemberShare == b.roomMemberShare &&
		a.roomMembersHidden == b.roomMembersHidden && a.roomChatHidden == b.roomChatHidden;
}
void SaveColumn(const netplay::PlayerPreferences& preferences) {
	RuntimeCommand request = Command(CommandKind::SavePreferences);
	request.roomSettings = true; request.preferences = preferences;
	NetplayFacade::SubmitRuntimeCommand(std::move(request));
}
// The settings file as the next start reads it: the column's keys hold the chosen values.
bool Written(const netplay::PlayerPreferences& chosen) {
	std::ifstream file(Environment(L"APPDATA") + L"\\sf4e\\settings.json");
	const auto json = nlohmann::json::parse(file, nullptr, false);
	if (json.is_discarded()) return false;
	const auto find = [&](const char* key) -> nlohmann::json {
		if (json.contains(key)) return json[key];
		for (const auto& value : json) if (value.is_object() && value.contains(key)) return value[key];
		return nullptr;
	};
	return find("roomSideWidth") == chosen.roomSideWidth && find("roomMemberShare") == chosen.roomMemberShare &&
		find("roomMembersHidden") == chosen.roomMembersHidden && find("roomChatHidden") == chosen.roomChatHidden;
}

}

bool StartRoomLayout() {
	s_layout.Enter(Stage::Setup);
	return true;
}

void TickRoomLayout() {
	RoomLayout& run = s_layout;
	run.waited++;
	const auto now = Now();
	switch (run.stage) {
	case Stage::Setup: {
		if (!online::SetupReady(run, *now)) break;
		run.before = now->preferences;
		RuntimeCommand request = Command(CommandKind::HostRoom);
		request.preferences = online::TestPreferences(*now, true);
		request.displayName = request.preferences.displayName;
		NetplayFacade::SubmitRuntimeCommand(std::move(request));
		run.Enter(Stage::Opening);
		break;
	}
	case Stage::Opening:
		if (now->session.room == netplay::RoomState::Joined) {
			Say(true, "room", now->room.name);
			Say(!now->canEditPreferences, "settings locked", now->canEditPreferences ? "the room left the settings open" : "");
			run.Enter(Stage::Save);
		}
		else if (!now->session.error.empty()) run.Fail("room", now->session.error);
		else if (run.Late(60)) run.Fail("room", "never joined");
		break;
	case Stage::Save:
		if (SameColumn(now->preferences, Chosen(run.before))) { Say(true, "column saved in the room"); run.Enter(Stage::Written); break; }
		if (run.Every(3)) SaveColumn(Chosen(run.before));
		if (run.Late(15)) run.Fail("column saved in the room", "the runtime never took it", Stage::Restore);
		break;
	case Stage::Written:
		// The file is written behind the runtime; give it a moment.
		if (Written(Chosen(run.before))) { Say(true, "column in settings.json"); run.Enter(Stage::Restore); break; }
		if (run.Late(10)) run.Fail("column in settings.json", "the file never held it", Stage::Restore);
		break;
	case Stage::Restore:
		if (SameColumn(now->preferences, run.before)) { Say(true, "column put back"); run.Enter(Stage::Leaving); break; }
		if (run.Every(3)) SaveColumn(run.before);
		if (run.Late(15)) run.Fail("column put back", "the runtime never took it", Stage::Leaving);
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
