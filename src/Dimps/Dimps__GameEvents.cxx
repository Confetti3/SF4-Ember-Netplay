#include <windows.h>
#include <cstdint>

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
MainMenu::__taskMethods MainMenu::taskMethods;
MainMenu::__exitMethods MainMenu::exitMethods;
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

GameEvents::OptionUIController::Methods GameEvents::OptionUIController::methods;
void GameEvents::OptionUIController::Locate(HMODULE module) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    // Steam 1.05: thiscall constructor takes two dwords (ret 8);
    // destructor takes only this (ret). Both run on the game thread.
    *(PVOID*)&methods.Construct = (PVOID)(base + 0xCC280);
    *(PVOID*)&methods.Destroy = (PVOID)(base + 0xCC0D0);
}
void GameEvents::Locate(HMODULE peRoot) {
    OptionUIController::Locate(peRoot);
	MainMenu::Locate(peRoot);
	RootEvent::Locate(peRoot);
	StageSelect::Locate(peRoot);
	VsBattle::Locate(peRoot);
	VsCharaSelect::Locate(peRoot);
	VsMode::Locate(peRoot);
	VsPreBattle::Locate(peRoot);
	VsStageSelect::Locate(peRoot);
	TrainingBattle::Locate(peRoot);
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

void GameEvents::Flow::Request(Flow* flow, int row) {
	struct Native { void Request(int row, int, int, int, int); };
	void (Native::* request)(int, int, int, int, int);
	*(PVOID*)&request = (*(PVOID**)flow)[RequestOffset / sizeof(PVOID)];
	(((Native*)flow)->*request)(row, 0, 0, 0, 1);
}

void MainMenu::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Destroy = (PVOID)(peRootOffset + 0x212f90);
	*(PVOID*)&itemObserverMethods.OnModeSelected = (PVOID)(peRootOffset + 0x214cb0);
	*(PVOID*)&itemObserverMethods.GetItemObserverState = (PVOID)(peRootOffset + 0x212c70);
	*(PVOID*)&itemObserverMethods.GoToVersusMode = (PVOID)(peRootOffset + 0x2131a0);
	*(PVOID*)&taskMethods.IdleTask = (PVOID)(peRootOffset + 0x213980);
	exitMethods.GetApp = (void* (*)())(peRootOffset + 0xcf60);
	*(PVOID*)&exitMethods.FadeOut = (PVOID)(peRootOffset + 0x286570);
	*(PVOID*)&exitMethods.Close = (PVOID)(peRootOffset + 0x2d6b70);
	exitMethods.fadeSeconds = (const float*)(peRootOffset + 0x538e00);
	staticMethods.Factory = (MainMenu*(*)(DWORD, DWORD, DWORD))(peRootOffset + 0x212f30);
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

void VsMode::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Destroy = (PVOID)(peRootOffset + 0x23dc60);

	staticMethods.Factory = (VsMode * (*)(DWORD, DWORD, DWORD))(peRootOffset + 0x23dde0);
}

static_assert(sizeof(GameEvents::TrainingMode::Selection) == 0x14, "Training's selection is 0x14 bytes a side");

GameEvents::TrainingMode::Selection* GameEvents::TrainingMode::GetSelection(TrainingMode* e, int side) {
	return (Selection*)((unsigned int)e + 0x60 + side * sizeof(Selection));
}

BYTE* GameEvents::TrainingBattle::stagePush = nullptr;
BYTE* GameEvents::TrainingBattle::stageMusicPush = nullptr;

void GameEvents::TrainingBattle::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	stagePush = (BYTE*)(peRootOffset + 0x238684);
	stageMusicPush = (BYTE*)(peRootOffset + 0x23868d);
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

void StageSelect::Control::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.GetPhase = (PVOID)(peRootOffset + 0x0d22c0);
	*(PVOID*)&publicMethods.SetStageCursor = (PVOID)(peRootOffset + 0x0d3670);
	*(PVOID*)&publicMethods.SelectStage = (PVOID)(peRootOffset + 0x0d25f0);
}
