#include "sf4e__SelfTest__Online.hxx"

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

// The room layout test: on one PC, in a private room it hosts, the room
// board's layout (its column, the members' share of it, which panels show)
// and the watched game's picture are saved the way the board, its grips and
// Settings save them in a room (RuntimeCommand::roomSettings,
// PlayerPreferences::CopyRoomSettings), since a room locks the player's other
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

// The layout the test saves: each value one the player could choose, and
// none of them what the player has now.
netplay::PlayerPreferences Chosen(const netplay::PlayerPreferences& now) {
	auto chosen = now;
	chosen.roomColumn = now.roomColumn == 44 ? 48 : 44;
	chosen.roomSplit = now.roomSplit == 62 ? 68 : 62;
	chosen.roomShowMembers = !now.roomShowMembers; chosen.roomShowChat = !now.roomShowChat;
	chosen.pictureMode = now.pictureMode == netplay::PictureBig ? netplay::PictureOff : netplay::PictureBig;
	chosen.pictureSpot = (now.pictureSpot + 5) % 12;
	return chosen;
}
bool SameLayout(const netplay::PlayerPreferences& a, const netplay::PlayerPreferences& b) {
	return a.roomColumn == b.roomColumn && a.roomSplit == b.roomSplit && a.roomShowMembers == b.roomShowMembers &&
		a.roomShowChat == b.roomShowChat && a.pictureMode == b.pictureMode && a.pictureSpot == b.pictureSpot;
}
void SaveLayout(const netplay::PlayerPreferences& preferences) {
	RuntimeCommand request = Command(CommandKind::SavePreferences);
	request.roomSettings = true; request.preferences = preferences;
	NetplayFacade::SubmitRuntimeCommand(std::move(request));
}
// The settings file as the next start reads it: the layout's keys hold the chosen values.
bool Written(const netplay::PlayerPreferences& chosen) {
	std::ifstream file(Environment(L"APPDATA") + L"\\sf4e\\settings.json");
	const auto json = nlohmann::json::parse(file, nullptr, false);
	if (json.is_discarded()) return false;
	const auto find = [&](const char* key) -> nlohmann::json {
		if (json.contains(key)) return json[key];
		for (const auto& value : json) if (value.is_object() && value.contains(key)) return value[key];
		return nullptr;
	};
	return find("roomColumn") == chosen.roomColumn && find("roomSplit") == chosen.roomSplit &&
		find("roomShowMembers") == chosen.roomShowMembers && find("roomShowChat") == chosen.roomShowChat &&
		find("pictureMode") == chosen.pictureMode && find("pictureSpot") == chosen.pictureSpot;
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
			Say(true, "room", now->room ? now->room->name : std::string());
			Say(!now->canEditPreferences, "settings locked", now->canEditPreferences ? "the room left the settings open" : "");
			run.Enter(Stage::Save);
		}
		else if (!now->session.error.empty()) run.Fail("room", now->session.error);
		else if (run.Late(60)) run.Fail("room", "never joined");
		break;
	case Stage::Save:
		if (SameLayout(now->preferences, Chosen(run.before))) { Say(true, "layout saved in the room"); run.Enter(Stage::Written); break; }
		if (run.Every(3)) SaveLayout(Chosen(run.before));
		if (run.Late(15)) run.Fail("layout saved in the room", "the runtime never took it", Stage::Restore);
		break;
	case Stage::Written:
		// The file is written behind the runtime; give it a moment.
		if (Written(Chosen(run.before))) { Say(true, "layout in settings.json"); run.Enter(Stage::Restore); break; }
		if (run.Late(10)) run.Fail("layout in settings.json", "the file never held it", Stage::Restore);
		break;
	case Stage::Restore:
		if (SameLayout(now->preferences, run.before)) { Say(true, "layout put back"); run.Enter(Stage::Leaving); break; }
		if (run.Every(3)) SaveLayout(run.before);
		if (run.Late(15)) run.Fail("layout put back", "the runtime never took it", Stage::Leaving);
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
