#include "sf4e__SelfTest__Online.hxx"

#include <atomic>

#include "../ui/MenuWalk.hxx"

// The menu test: turns the Look to V2 and walks the menus by the keys a player
// presses (ui::MenuWalk: Home, Settings and a value's popup, Online play, a
// room created, its member popout, the room left, Training from Home). The
// walk runs on the overlay's frames (MenuFrame); the game thread readies it,
// checks that Training came in front and puts the Look back.
namespace sf4e { namespace selftest {
namespace {

using netplay::CommandKind;
using online::Command;
using online::Now;

// Done: the outcome is written (Finish).
enum class Stage { Setup, Look, Walking, Training, Restore, Done };

struct Menu : Steps<Stage> {
	bool lookBefore = false;
} s_menu;

// Handed between the game thread and the overlay's: the walk starts once the
// Look is V2, and its steps are said before `walked` is set.
std::atomic<bool> s_walking{false}, s_walked{false};
ui::MenuWalk s_walk;

void SaveLook(bool v2) {
	auto request = Command(CommandKind::SavePreferences);
	request.preferences.lookV2 = v2;
	NetplayFacade::SubmitRuntimeCommand(std::move(request));
}

}

bool StartMenu() {
	s_menu.Enter(Stage::Setup);
	return true;
}

bool MenuFrame(const ui::MenuNavigation& nav, unsigned& buttons) {
	if (!s_walking || s_walked) return false;
	buttons |= s_walk.Frame(nav, [](bool pass, const std::string& step, const std::string& detail) { Say(pass, step, detail); });
	if (s_walk.Done()) s_walked = true;
	return !s_walked;
}

void TickMenu(bool atMainMenu) {
	Menu& run = s_menu;
	run.waited++;
	const auto now = Now();
	switch (run.stage) {
	case Stage::Setup:
		// A room needs a controller and the helper.
		if (!online::SetupReady(run, *now)) break;
		run.lookBefore = now->preferences.lookV2;
		run.Enter(Stage::Look);
		break;
	case Stage::Look:
		if (now->preferences.lookV2) { Say(true, "look V2"); s_walking = true; run.Enter(Stage::Walking); break; }
		if (run.Every(3)) SaveLook(true);
		if (run.Late(15)) run.Fail("look V2", "the preference was not taken");
		break;
	case Stage::Walking:
		if (!s_walked) break;
		run.Enter(s_walk.Failed() ? Stage::Restore : Stage::Training);
		break;
	case Stage::Training:
		// Training's Select leaves the main menu for the training battle.
		if (!atMainMenu && run.Late(5)) { Say(true, "training in front"); run.Enter(Stage::Restore); }
		else if (run.Late(60)) run.Fail("training in front", "the main menu never left", Stage::Restore);
		break;
	case Stage::Restore:
		// The player's own Look again; Training lets preferences be saved.
		if (now->preferences.lookV2 == run.lookBefore) { Say(true, "look restored"); run.Enter(Stage::Done); Finish(); break; }
		if (run.Every(3)) SaveLook(run.lookBefore);
		if (run.Late(15)) run.Fail("look restored", "the preference was not taken");
		break;
	case Stage::Done:
		break;
	}
}

} }
