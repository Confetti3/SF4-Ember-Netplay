#include <windows.h>
#include <detours/detours.h>
#include <atomic>
#include <string>

#include "spdlog/spdlog.h"

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../common/InputLagControl.hxx"
#include "../common/InputLagEdit.hxx"
#include "sf4e__CodePages.hxx"
#include "sf4e__InputLag.hxx"

using rKeyboards = Dimps::Pad::System_RawInput;
namespace fInputLag = sf4e::InputLag;
namespace control = sf4e::input_lag;

namespace {
// The keyboard hook is queued, then committed. Until then the keyboard row
// does nothing and the game keeps its own timing.
bool attached = false;
std::atomic<bool> hooked{false};

control::Battle battle;
control::Keyboard keyboard;

sf4e::code_edit::Edit Store() { return control::StoreAtReadFrame(Dimps::Game::Battle::Network::inputStoreFrame); }

void LogBattle(const sf4e::code_edit::Edit& store, const char* source, const std::string& seats) {
	spdlog::info("Input lag: battle P1={} source={} seats={} keyboard={}", sf4e::code_edit::Reads(store, true) ? "on" : "off",
		source, seats, keyboard.First() ? "on" : "off");
}

struct Keyboards : rKeyboards {
	void Update();
};

// The device loop's pass, unless the pad update already took this frame's keys.
void Keyboards::Update() {
	if (keyboard.DeviceLoopPass()) (this->*rKeyboards::publicMethods.Update)();
}
}

void fInputLag::Install() {
	void (Keyboards::* update)() = &Keyboards::Update;
	attached = DetourAttach((PVOID*)&rKeyboards::publicMethods.Update, *(PVOID*)&update) == NO_ERROR;
}

void fInputLag::Activate() {
	hooked.store(attached);
}

bool fInputLag::StartMatch(bool on, const bool (&seats)[2]) {
	const auto store = Store();
	sf4e::CodePages pages;
	const bool established = battle.StartMatch(store, on, pages);
	LogBattle(store, "match", std::string(seats[0] ? "on" : "off") + "/" + (seats[1] ? "on" : "off"));
	if (!established)
		spdlog::error("Input lag: the match needs the battle's input store {}, and it could not be set; the match does not start",
			on ? "lowered" : "native");
	return established;
}

void fInputLag::BeforeBattleFrame(bool setting) {
	const auto store = Store();
	sf4e::CodePages pages;
	if (!battle.BeforeFrame(store, setting, pages)) return;
	LogBattle(store, "setting", "-");
	if (!sf4e::code_edit::Reads(store, setting))
		spdlog::warn("Input lag: the battle's input store could not follow the setting; this battle plays as it reads");
}

void fInputLag::CloseBattle() {
	const auto store = Store();
	sf4e::CodePages pages;
	if (!battle.Close(store, pages))
		spdlog::warn("Input lag: the battle's input store could not be put back; matches that need it native will not start");
}

bool fInputLag::BattleInputLowered() {
	return sf4e::code_edit::Reads(Store(), true);
}

void fInputLag::BeforePadUpdate(bool setting) {
	// The game's own pass, through the hook's trampoline. It still waits for
	// the pad update's request (+0x10), so the keyboard stays frozen while the
	// Steam overlay is open. The manager is a static object, never null.
	if (keyboard.BeforePadUpdate(setting, hooked.load())) {
		auto* keyboards = rKeyboards::staticMethods.GetSingleton();
		(keyboards->*rKeyboards::publicMethods.Update)();
	}
}
