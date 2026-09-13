#include <windows.h>

#include "Dimps__Event.hxx"
#include "Dimps__Game.hxx"
#include "Dimps__GameEvents.hxx"
#include "Dimps__Platform.hxx"

namespace GameEvents = Dimps::GameEvents;
namespace StageSelect = Dimps::GameEvents::StageSelect;

using Dimps::Event::EventBaseWithEC;
using Dimps::Event::EventController;
using Dimps::Game::Request;
using GameEvents::MainMenu;
using GameEvents::RootEvent;
using GameEvents::VsBattle;
using GameEvents::VsCharaSelect;
using GameEvents::VsMode;
using GameEvents::VsPreBattle;
using GameEvents::VsStageSelect;
using Dimps::Platform::dString;

MainMenu::__itemObserverMethods MainMenu::itemObserverMethods;
MainMenu::__publicMethods MainMenu::publicMethods;
MainMenu::__staticMethods MainMenu::staticMethods;
char** RootEvent::eventFlowDefinition;
VsBattle::__privateMethods VsBattle::privateMethods;
VsBattle::__publicMethods VsBattle::publicMethods;
VsCharaSelect::__publicMethods VsCharaSelect::publicMethods;
VsCharaSelect::__staticMethods VsCharaSelect::staticMethods;
VsMode::__publicMethods VsMode::publicMethods;
VsMode::__staticMethods VsMode::staticMethods;
BOOL(VsBattle::** VsBattle::vt_IsTerminationComplete)();
VsPreBattle::__publicMethods VsPreBattle::publicMethods;
VsStageSelect::__publicMethods VsStageSelect::publicMethods;
VsStageSelect::__staticMethods VsStageSelect::staticMethods;
StageSelect::Control::__publicMethods StageSelect::Control::publicMethods;

void GameEvents::Locate(HMODULE peRoot) {
	MainMenu::Locate(peRoot);
	RootEvent::Locate(peRoot);
	StageSelect::Locate(peRoot);
	VsBattle::Locate(peRoot);
	VsCharaSelect::Locate(peRoot);
	VsMode::Locate(peRoot);
	VsPreBattle::Locate(peRoot);
	VsStageSelect::Locate(peRoot);
}

void StageSelect::Locate(HMODULE peRoot) {
	Control::Locate(peRoot);
}

void VsBattle::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	vt_IsTerminationComplete = (BOOL(VsBattle::**)())(peRootOffset + 0x55f878);
	*(PVOID*)&privateMethods.CheckAndMaybeExitBasedOnExitType = (PVOID)(peRootOffset + 0x23b9f0);
	*(PVOID*)&privateMethods.PrepareBattleRequest = (PVOID)(peRootOffset + 0x23c6a0);
	*(PVOID*)&publicMethods.HasInitialized = (PVOID)(peRootOffset + 0x23bd90);
	*(PVOID*)&publicMethods.RegisterTasks = (PVOID)(peRootOffset + 0x23cad0);
	*(PVOID*)&publicMethods.ExitForeground = (PVOID)(peRootOffset + 0x23bdf0);
}

Request** VsBattle::GetRequest(VsBattle* battleEvent) {
	return (Request**)((unsigned int)battleEvent + 0x50);
}

void MainMenu::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Destroy = (PVOID)(peRootOffset + 0x212f90);
	*(PVOID*)&itemObserverMethods.OnModeSelected = (PVOID)(peRootOffset + 0x214cb0);
	*(PVOID*)&itemObserverMethods.GetItemObserverState = (PVOID)(peRootOffset + 0x212c70);
	*(PVOID*)&itemObserverMethods.GoToVersusMode = (PVOID)(peRootOffset + 0x2131a0);
	staticMethods.Factory = (MainMenu*(*)(DWORD, DWORD, DWORD))(peRootOffset + 0x212f30);
}


MainMenu* MainMenu::ToItemObserver(MainMenu* s) {
	return (MainMenu*)((unsigned int)s + 0x40);
}

MainMenu* MainMenu::FromItemObserver(MainMenu* s) {
	return (MainMenu*)((unsigned int)s - 0x40);
}

Dimps::Game::ProgressData** RootEvent::GetProgressData(RootEvent* e) {
	return (Dimps::Game::ProgressData**)((unsigned int)e + 0x6c);
}

void RootEvent::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	eventFlowDefinition = (char**)(peRootOffset + 0x665958);
}

void VsCharaSelect::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Destroy = (PVOID)(peRootOffset + 0x23d0e0);

	staticMethods.Factory = (VsCharaSelect * (*)(DWORD, DWORD, DWORD))(peRootOffset + 0x23d660);
}

VsCharaSelect::CharaSelectState* VsCharaSelect::GetState(VsCharaSelect* selectEvent) {
	return (CharaSelectState*)((unsigned int)selectEvent + 0x4c);
}

dString* VsCharaSelect::PlayerConditions::GetSelectedCharaAbbrev(PlayerConditions* c) {
	return (dString*)((unsigned int)c + 0x20);
}

dString* VsCharaSelect::PlayerConditions::GetHoveredCharaAbbrev(PlayerConditions* c) {
	return (dString*)((unsigned int)c + 0x3c);
}

BYTE* VsCharaSelect::PlayerConditions::GetUltraCombo(PlayerConditions* c) {
	return (BYTE*)((unsigned int)c + 0x58);
}

BYTE* VsCharaSelect::PlayerConditions::GetColor(PlayerConditions* c) {
	return (BYTE*)((unsigned int)c + 0x59);
}

BYTE* VsCharaSelect::PlayerConditions::GetCostume(PlayerConditions* c) {
	return (BYTE*)((unsigned int)c + 0x5a);
}

BYTE* VsCharaSelect::PlayerConditions::GetPersonalAction(PlayerConditions* c) {
	return (BYTE*)((unsigned int)c + 0x5b);
}

BYTE* VsCharaSelect::PlayerConditions::GetWinQuote(PlayerConditions* c) {
	return (BYTE*)((unsigned int)c + 0x5c);
}

BYTE* VsCharaSelect::PlayerConditions::GetHandicap(PlayerConditions* c) {
	return (BYTE*)((unsigned int)c + 0x5d);
}

BYTE* VsCharaSelect::PlayerConditions::GetEdition(PlayerConditions* c) {
	return (BYTE*)((unsigned int)c + 0x5e);
}

void VsMode::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Destroy = (PVOID)(peRootOffset + 0x23dc60);

	staticMethods.Factory = (VsMode * (*)(DWORD, DWORD, DWORD))(peRootOffset + 0x23dde0);
}

VsMode::ConfirmedPlayerConditions* VsMode::GetConfirmedPlayerConditions(VsMode* e) {
	return (VsMode::ConfirmedPlayerConditions*)((unsigned int)e + 0x58);
}

dString* VsMode::GetStageName(VsMode* e) {
	return (dString*)((unsigned int)e + 0x2d0);
}

int* VsMode::GetStageCode(VsMode* e) {
	return (int*)((unsigned int)e + 0x2ec);
}

VsMode::ConfirmedCharaConditions* VsMode::ConfirmedPlayerConditions::GetCharaConditions(VsMode::ConfirmedPlayerConditions* c) {
	return (VsMode::ConfirmedCharaConditions*)((unsigned int)c + 0x128);
}

int* VsMode::ConfirmedPlayerConditions::GetCharaID(VsMode::ConfirmedPlayerConditions* c) {
	return (int*)((unsigned int)c + 0x134);
}

int* VsMode::ConfirmedPlayerConditions::GetSideActive(VsMode::ConfirmedPlayerConditions* c) {
	return (int*)((unsigned int)c + 0x138);
}

void VsPreBattle::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.RegisterTasks = (PVOID)(peRootOffset + 0x23e1a0);
}

void VsStageSelect::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Destroy = (PVOID)(peRootOffset + 0x23e5d0);
	staticMethods.Factory = (VsStageSelect * (*)(DWORD, DWORD, DWORD))(peRootOffset + 0x23e870);
}

VsStageSelect::StageSelectState* VsStageSelect::GetState(VsStageSelect* selectEvent) {
	return (StageSelectState*)((unsigned int)selectEvent + 0x48);
}

StageSelect::Control* VsStageSelect::GetControl(VsStageSelect* selectEvent) {
	return *(StageSelect::Control**)((unsigned int)selectEvent + 0x44);
}

void StageSelect::Control::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.GetPhase = (PVOID)(peRootOffset + 0x0d22c0);
	*(PVOID*)&publicMethods.SetStageCursor = (PVOID)(peRootOffset + 0x0d3670);
	*(PVOID*)&publicMethods.SelectStage = (PVOID)(peRootOffset + 0x0d25f0);
}
