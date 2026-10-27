#include <windows.h>

#include "Dimps__Eva.hxx"
#include "Dimps__Game.hxx"
#include "Dimps__Game__Battle.hxx"
#include "Dimps__Game__Battle__Hud.hxx"
#include "Dimps__Platform.hxx"

using Dimps::Eva::IEmSpriteNode;
using Dimps::Eva::Task;
using Dimps::Game::Battle::IUnit;
using Dimps::Game::Sprite::Control;
using Dimps::Game::Sprite::SingleNodeControl;
using Dimps::Platform::dDeque_0x10;
using Dimps::Platform::WithReleaser;

namespace Hud = Dimps::Game::Battle::Hud;
using Hud::Unit;

Unit::__publicMethods Unit::publicMethods;
Unit::__staticMethods Unit::staticMethods;

Hud::Announce::Round::__publicMethods Hud::Announce::Round::publicMethods;
Hud::Announce::Unit::__publicMethods Hud::Announce::Unit::publicMethods;
Hud::Cockpit::Unit::__publicMethods Hud::Cockpit::Unit::publicMethods;
Hud::Cockpit::View::__publicMethods Hud::Cockpit::View::publicMethods;
Hud::Cursor::Unit::__publicMethods Hud::Cursor::Unit::publicMethods;
Hud::Notice::Bonus::__publicMethods Hud::Notice::Bonus::publicMethods;
Hud::Notice::Combo::__publicMethods Hud::Notice::Combo::publicMethods;
Hud::Notice::Unit::__publicMethods Hud::Notice::Unit::publicMethods;
Hud::Result::Unit::__publicMethods Hud::Result::Unit::publicMethods;
Hud::Subtitle::Unit::__publicMethods Hud::Subtitle::Unit::publicMethods;
Hud::Training::Unit::__publicMethods Hud::Training::Unit::publicMethods;

void (IUnit::* Hud::SharedHudUpdate)(Task** task);

void Hud::Locate(HMODULE peRoot) {
	Unit::Locate(peRoot);
	Announce::Round::Locate(peRoot);
	Announce::Unit::Locate(peRoot);
	Cockpit::Unit::Locate(peRoot);
	Cockpit::View::Locate(peRoot);
	Cursor::Unit::Locate(peRoot);
	Notice::Bonus::Locate(peRoot);
	Notice::Combo::Locate(peRoot);
	Notice::Unit::Locate(peRoot);
	Result::Unit::Locate(peRoot);
	Subtitle::Unit::Locate(peRoot);
	Training::Unit::Locate(peRoot);

	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&SharedHudUpdate = (PVOID)(peRootOffset + 0x18ae90);
}

void Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.RecordToInternalMementoKey = (PVOID)(peRootOffset + 0x172e60);
	*(PVOID*)&publicMethods.RestoreFromInternalMementoKey = (PVOID)(peRootOffset + 0x172e90);
	staticMethods.Factory = (Unit*(*)(DWORD, DWORD))(peRootOffset + 0x1731a0);
}

void Hud::Announce::Round::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Disable = (PVOID)(peRootOffset + 0x0);
	*(PVOID*)&publicMethods.Enable = (PVOID)(peRootOffset + 0x0);
}

void Hud::Announce::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.HudAnnounce_Update = (PVOID)(peRootOffset + 0x180b90);
}

Hud::Announce::View** Hud::Announce::Unit::GetView(Unit* u) {
	return (View**)((unsigned int)u + 0x38);
}

void Hud::Cockpit::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.HudCockpit_Update = (PVOID)(peRootOffset + 0x173f00);
}

void Hud::Cockpit::View::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.RestoreFromInternalMementoKey = (PVOID)(peRootOffset + 0x174c70);
	*(PVOID*)&publicMethods.Update = (PVOID)(peRootOffset + 0x1764b0);
}

void Hud::Cursor::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.HudCursor_Update = (PVOID)(peRootOffset + 0x18c130);
}

void Hud::Notice::Bonus::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Enable = (PVOID)(peRootOffset + 0x189770);
	*(PVOID*)&publicMethods.Disable = (PVOID)(peRootOffset + 0x1898c0);
}

int32_t* Hud::Notice::Bonus::GetScore(Bonus* b) {
	return (int32_t*)((unsigned int)b + 0x78);
}

DWORD* Hud::Notice::Bonus::GetIsActive(Bonus* b) {
	return (DWORD*)((unsigned int)b + 0x7c);
}

void Hud::Notice::Combo::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Enable = (PVOID)(peRootOffset + 0x18a0e0);
}

int32_t* Hud::Notice::Combo::GetScore(Combo* c) {
	return (int32_t*)((unsigned int)c + 0x64);
}

DWORD* Hud::Notice::Combo::GetIsActive(Combo* c) {
	return (DWORD*)((unsigned int)c + 0x68);
}

void Hud::Notice::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.RestoreFromInternalMementoKey = (PVOID)(peRootOffset + 0x1886c0);
}

Hud::Notice::View** Hud::Notice::Unit::GetView(Unit* u) {
	return (View**)((unsigned int)u + 0x18);
}

void Hud::Result::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.HudResult_Update = (PVOID)(peRootOffset + 0x1834d0);
}

void Hud::Subtitle::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.HudSubtitle_Update = (PVOID)(peRootOffset + 0x18e2c0);
}

void Hud::Training::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.HudTraining_Update = (PVOID)(peRootOffset + 0x18f290);
}

void Unit::ResetUnits(Unit* u) {
	*GetCockpit(u) = nullptr;
	*GetNotice(u) = nullptr;
	*GetAnnounce(u) = nullptr;
	*GetCursor(u) = nullptr;
	*GetResult(u) = nullptr;
	*GetContinue(u) = nullptr;
	*GetSubtitle(u) = nullptr;
	*GetTraining(u) = nullptr;
}
