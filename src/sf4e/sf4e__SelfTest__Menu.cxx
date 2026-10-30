#include "sf4e__SelfTest__Online.hxx"

#include <atomic>

#include "../ui/MenuWalk.hxx"

// The menu test: walks the menus by the keys a player presses (ui::MenuWalk:
// Home, Settings and a value's popup, Online play, a room created, its member
// popout, the room left, Training from Home). The walk runs on the overlay's
// frames (MenuFrame); the game thread readies it and checks that Training
// came in front.
namespace sf4e { namespace selftest {
namespace {

using online::Now;

// Done: the outcome is written (Finish).
enum class Stage { Setup, Walking, Training, Done };

struct Menu : Steps<Stage> {} s_menu;

// Handed between the game thread and the overlay's: the walk starts once the
// setup is ready, and its steps are said before `walked` is set.
std::atomic<bool> s_walking{false}, s_walked{false};
ui::MenuWalk s_walk;

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
		s_walking = true;
		run.Enter(Stage::Walking);
		break;
	case Stage::Walking:
		if (!s_walked) break;
		if (s_walk.Failed()) { run.Enter(Stage::Done); Finish(); break; }
		run.Enter(Stage::Training);
		break;
	case Stage::Training:
		// Training's Select leaves the main menu for the training battle.
		if (!atMainMenu && run.Late(5)) { Say(true, "training in front"); run.Enter(Stage::Done); Finish(); }
		else if (run.Late(60)) run.Fail("training in front", "the main menu never left");
		break;
	case Stage::Done:
		break;
	}
}

} }
