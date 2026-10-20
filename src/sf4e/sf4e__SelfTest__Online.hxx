#pragma once

#include <memory>
#include <string>

#include "sf4e__NetplayFacade.hxx"
#include "sf4e__SelfTest.hxx"

// What the two-PC tests share (sf4e__SelfTest__Room.cxx and
// sf4e__SelfTest__Tournament.cxx): the runtime's commands as the screens build
// them, the setup both need, Ready as Ryu, and files the two sides pass lines
// through. Included only by those tests; the header the game includes is
// sf4e__SelfTest.hxx.
namespace sf4e { namespace selftest { namespace online {

std::shared_ptr<const NetplayFacade::RuntimeSnapshot> Now();

// A command as the room's controls build it, tagged with the current
// generation so a stale one is dropped as a stale press would be.
NetplayFacade::RuntimeCommand Command(netplay::CommandKind kind);

// Each side its own name, since a room refuses a second member of the same
// name; a fixed delay, so Ready does not wait for the connection check; one
// round. `host` is the side that takes Player 1's seat.
netplay::PlayerPreferences TestPreferences(const NetplayFacade::RuntimeSnapshot& now, bool host);

// Asks for the keyboard as the controller page does: a room needs a controller.
void UseKeyboard(const NetplayFacade::RuntimeSnapshot& now);

// Ready as Ryu on the first stage, once both seats of `table` are filled and
// nothing is pending. Sent again by the caller every few seconds: a Ready
// parked while the room settles is dropped.
void ReadyAsRyu(const NetplayFacade::RuntimeSnapshot& now, int table);

// The first line of a file, without its line end; empty until it was written.
std::string ReadLine(const std::wstring& path);
// Written whole or not at all, through a temp file and a rename, so the other
// side never reads half a line.
bool WriteLine(const std::wstring& path, const std::string& line);

// Setup, the same for both tests: a controller (the keyboard, when none was
// kept) and the helper, which a room is opened with. True once both are
// there; failing either ends the run. `Stage` is the test's, with Done.
template <class Stage>
bool SetupReady(Steps<Stage>& run, const NetplayFacade::RuntimeSnapshot& now) {
	if (!now.controllerReady) {
		if (run.Every(3)) UseKeyboard(now);
		if (run.Late(15)) run.Fail("controller", "none could be assigned");
		return false;
	}
	if (!now.helperReady || !now.canOpenRoom) {
		if (run.Late(60)) run.Fail("helper", "rooms never became available: " + now.helperError);
		return false;
	}
	return true;
}

} } }
