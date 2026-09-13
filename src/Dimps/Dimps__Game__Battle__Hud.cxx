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

Task** Hud::Announce::Unit::GetHudAnnounceUpdateTask(Unit* u) {
	return (Task**)((unsigned int)u + 0x8);
}

dDeque_0x10* Hud::Announce::View::GetQueuedAnnouncements(View* v) {
	return (dDeque_0x10*)((unsigned int)v + 0xb8);
}

Dimps::Game::Sprite::Control** Hud::Announce::View::GetActiveControl(View* v) {
	return (Dimps::Game::Sprite::Control**)((unsigned int)v + 0xd0);
}

WORD* Hud::Announce::Round::GetCurrentRound(Round* c) {
	return (WORD*)((unsigned int)c + 0x2c);
}

IEmSpriteNode** Hud::Announce::Round::GetRoundActionNode(Round* c) {
	return (IEmSpriteNode**)((unsigned int)c + 0x1c);
}

IEmSpriteNode** Hud::Announce::Round::GetRoundNumberNode(Round* c) {
	return (IEmSpriteNode**)((unsigned int)c + 0x24);
}

Hud::Announce::Round** Hud::Announce::View::GetRoundControl(View* v) {
	return (Round**)((unsigned int)v + 0x20);
}

SingleNodeControl** Hud::Announce::View::GetFinalRoundControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x28);
}

SingleNodeControl** Hud::Announce::View::GetExtraRoundControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x30);
}

SingleNodeControl** Hud::Announce::View::GetFightControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x38);
}

SingleNodeControl** Hud::Announce::View::GetKoControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x40);
}

SingleNodeControl** Hud::Announce::View::GetDoubleKoControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x48);
}

SingleNodeControl** Hud::Announce::View::GetTimeControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x50);
}

SingleNodeControl** Hud::Announce::View::GetDrawControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x58);
}

SingleNodeControl** Hud::Announce::View::GetPerfectControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x60);
}

SingleNodeControl** Hud::Announce::View::GetWinControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x68);
}

SingleNodeControl** Hud::Announce::View::GetLoseControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x70);
}

SingleNodeControl** Hud::Announce::View::GetWins1Control(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x78);
}

SingleNodeControl** Hud::Announce::View::GetWins2Control(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x80);
}

SingleNodeControl** Hud::Announce::View::GetSuccessControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x88);
}

SingleNodeControl** Hud::Announce::View::GetClearControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x90);
}

SingleNodeControl** Hud::Announce::View::GetReadyGoControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0x98);
}

SingleNodeControl** Hud::Announce::View::GetEditionControl(View* v) {
	return (SingleNodeControl**)((unsigned int)v + 0xa0);
}

Task** Hud::Cockpit::Unit::GetHudCockpitUpdateTask(Unit* u) {
	return (Task**)((unsigned int)u + 0x8);
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

Task** Hud::Continue::Unit::GetHudContinueUpdateTask(Unit* u) {
	return (Task**)((unsigned int)u + 0x8);
}

Task** Hud::Cursor::Unit::GetHudCursorUpdateTask(Unit* u) {
	return (Task**)((unsigned int)u + 0x8);
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

IEmSpriteNode** Hud::Notice::Bonus::GetActiveSprite(Bonus* b) {
	return (IEmSpriteNode**)((unsigned int)b + 0x64);
}

DWORD* Hud::Notice::Bonus::GetCurrentBonus(Bonus* b) {
	return (DWORD*)((unsigned int)b + 0x74);
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

uint32_t* Hud::Notice::Combo::GetComboLength(Combo* c) {
	return (uint32_t*)((unsigned int)c + 0x60);
}

int32_t* Hud::Notice::Combo::GetScore(Combo* c) {
	return (int32_t*)((unsigned int)c + 0x64);
}

DWORD* Hud::Notice::Combo::GetIsActive(Combo* c) {
	return (DWORD*)((unsigned int)c + 0x68);
}

DWORD* Hud::Notice::Combo::GetShouldShowAdjective(Combo* c) {
	return (DWORD*)((unsigned int)c + 0x6c);
}

WithReleaser<Hud::Notice::Bonus>* Hud::Notice::Player::GetBonuses(Player* p) {
	return (WithReleaser<Hud::Notice::Bonus>*)((unsigned int)p + 0x28);
}

WithReleaser<Hud::Notice::Combo>* Hud::Notice::Player::GetCombo(Player* p) {
	return (WithReleaser<Hud::Notice::Combo>*)((unsigned int)p + 0x20);
}

dDeque_0x10* Hud::Notice::Player::GetQueuedBonusNotices(Player* p) {
	return (dDeque_0x10*)((unsigned int)p + 0x50);
}

dDeque_0x10* Hud::Notice::Player::GetQueuedComboNotices(Player* p) {
	return (dDeque_0x10*)((unsigned int)p + 0x38);

}

void Hud::Notice::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.RestoreFromInternalMementoKey = (PVOID)(peRootOffset + 0x1886c0);
}

Task** Hud::Notice::Unit::GetHudNoticeUpdateTask(Unit* u) {
	return (Task**)((unsigned int)u + 0x8);
}

Hud::Notice::View** Hud::Notice::Unit::GetView(Unit* u) {
	return (View**)((unsigned int)u + 0x18);
}

Task** Hud::Result::Unit::GetHudResultUpdateTask(Unit* u) {
	return (Task**)((unsigned int)u + 0x8);
}

WithReleaser<Hud::Notice::Player>* Hud::Notice::View::GetPlayers(View* v) {
	return (WithReleaser<Hud::Notice::Player>*)((unsigned int)v + 0x20);
}

void Hud::Result::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.HudResult_Update = (PVOID)(peRootOffset + 0x1834d0);
}

Task** Hud::Subtitle::Unit::GetHudSubtitleUpdateTask(Unit* u) {
	return (Task**)((unsigned int)u + 0x8);
}

void Hud::Subtitle::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.HudSubtitle_Update = (PVOID)(peRootOffset + 0x18e2c0);
}

Task** Hud::Training::Unit::GetHudTrainingUpdateTask(Unit* u) {
	return (Task**)((unsigned int)u + 0x8);
}

void Hud::Training::Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.HudTraining_Update = (PVOID)(peRootOffset + 0x18f290);
}

Hud::Announce::Unit** Unit::GetAnnounce(Unit* u) {
	return (Announce::Unit**)((unsigned int)u + 0x20);
}

Hud::Cockpit::Unit** Unit::GetCockpit(Unit* u) {
	return (Cockpit::Unit**)((unsigned int)u + 0x18);
}

Hud::Continue::Unit** Unit::GetContinue(Unit* u) {
	return (Continue::Unit**)((unsigned int)u + 0x2c);
}

Hud::Cursor::Unit** Unit::GetCursor(Unit* u) {
	return (Cursor::Unit**)((unsigned int)u + 0x24);
}

Hud::Notice::Unit** Unit::GetNotice(Unit* u) {
	return (Notice::Unit**)((unsigned int)u + 0x1c);
}

Hud::Result::Unit** Unit::GetResult(Unit* u) {
	return (Result::Unit**)((unsigned int)u + 0x28);
}

Hud::Subtitle::Unit** Unit::GetSubtitle(Unit* u) {
	return (Subtitle::Unit**)((unsigned int)u + 0x30);
}

Hud::Training::Unit** Unit::GetTraining(Unit* u) {
	return (Training::Unit**)((unsigned int)u + 0x34);
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
