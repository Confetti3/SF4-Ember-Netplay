#include <windows.h>
#include <detours/detours.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "sf4e__Event.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__UserApp.hxx"

using Dimps::Game::Request;

namespace rGameEvents = Dimps::GameEvents;
using Dimps::Event::EventBase;
using Dimps::Event::EventBaseWithEC;
using Dimps::Event::EventController;
using rMainMenu = rGameEvents::MainMenu;
using rRootEvent = rGameEvents::RootEvent;
using rVsBattle = rGameEvents::VsBattle;
using rVsCharaSelect = rGameEvents::VsCharaSelect;
using rVsMode = rGameEvents::VsMode;
using rVsPreBattle = rGameEvents::VsPreBattle;
using rVsStageSelect = rGameEvents::VsStageSelect;

namespace fGameEvents = sf4e::GameEvents;
using fMainMenu = fGameEvents::MainMenu;
using fRootEvent = fGameEvents::RootEvent;
using fVsBattle = fGameEvents::VsBattle;
using fVsPreBattle = fGameEvents::VsPreBattle;
using fVsStageSelect = fGameEvents::VsStageSelect;
using fUserApp = sf4e::UserApp;

int (*fMainMenu::OnModeSelectedOverride)(int mode);
int fMainMenu::bOverrideItemObserverState = -1;
void (*fVsBattle::OnTasksRegistered)() = nullptr;
void (*fVsPreBattle::OnTasksRegistered)() = nullptr;

bool fVsBattle::bBlockInitialization = false;
bool fVsBattle::bBlockTermination = false;
bool fVsBattle::bForceNextMatchOnline = false;
bool fVsBattle::bSessionSentLoaded = false;
bool fVsBattle::bSessionSynced = false;
bool fVsBattle::bOverrideNextRandomSeed = false;
bool fVsBattle::bTerminateOnNextLeftBattle = false;
DWORD fVsBattle::nextMatchRandomSeed = 0xffffffff;
bool fVsPreBattle::bSkipToVersus = false;

char* fRootEvent::eventFlowDescription = R"(	Boot, 0, Title,										
LogoCapcom, 0, LogoNvidia, BLACK, 10.0f, BLACK, 10.0f			
LogoCapcom, 1, Title, BLACK, 10.0f, BLACK, 10.0f			
	LogoNvidia, 0, LogoDolby, BLACK, 10.0f, BLACK, 10.0f			
	LogoNvidia, 1, Title, BLACK, 10.0f, BLACK, 10.0f			
LogoDolby, 0, LogoCRI, BLACK, 10.0f, BLACK, 10.0f			
	LogoDolby, 1, Title, BLACK, 10.0f, BLACK, 10.0f			
	LogoCRI, 0, LogoScaleform, BLACK, 10.0f, BLACK, 10.0f			
	LogoCRI, 1, Title, BLACK, 10.0f, BLACK, 10.0f			
	LogoScaleform, 0, Opening, BLACK, 10.0f, BLACK, 10.0f			
	LogoScaleform, 1, Title, BLACK, 10.0f, BLACK, 10.0f			
	Title, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	Title, 1, DemoBattle, BLACK, 30.0f, BLACK, 30.0f			
	DemoBattle, 0, LogoCapcom, BLACK, 30.0f, BLACK, 30.0f			
	DemoBattle, 1, Title, BLACK, 30.0f, BLACK, 30.0f			
	Opening, 0, Title, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 0, Title, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 1, ArcadeMode, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 2, VSMode, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 3, NetworkMode, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 4, TrainingMode, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 5, Option, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 6, Marketplace, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 7, PlayerData, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 8, TrialMode, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 9, BonusStageMode1, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 10, BonusStageMode2, BLACK, 30.0f, BLACK, 30.0f			
	MainMenu, 11, PlayerTitleSetting, BLACK, 30.0f, WHITE, 30.0f			
	MainMenu, 12, PlayerIconSetting, BLACK, 30.0f, WHITE, 30.0f			
MainMenu, 14, Benchmark, BLACK, 30.0f, BLACK, 30.0f			
	Benchmark, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
MainMenu, 13, Manual, BLACK, 30.0f, BLACK, 30.0f			
	Manual, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	PlayerTitleSetting, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	PlayerTitleSetting, 1, Option, BLACK, 30.0f, BLACK, 30.0f			
	PlayerTitleSetting, 2, PlayerData, BLACK, 30.0f, BLACK, 30.0f			
	PlayerTitleSetting, 3, NetworkMode, BLACK, 30.0f, BLACK, 30.0f			
	PlayerTitleSetting, 4, PlayerIconSetting, BLACK, 30.0f, BLACK, 30.0f			
	PlayerIconSetting, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	PlayerIconSetting, 1, Option, BLACK, 30.0f, BLACK, 30.0f			
	PlayerIconSetting, 2, PlayerData, BLACK, 30.0f, BLACK, 30.0f			
	PlayerIconSetting, 3, NetworkMode, BLACK, 30.0f, BLACK, 30.0f			
	PlayerIconSetting, 4, PlayerTitleSetting, BLACK, 30.0f, BLACK, 30.0f			
	ArcadeMode, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	VSMode, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	NetworkMode, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	TrainingMode, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	Option, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	Option, 1, PlayerTitleSetting, BLACK, 30.0f, BLACK, 30.0f			
	Option, 2, PlayerIconSetting, BLACK, 30.0f, BLACK, 30.0f			
	Marketplace, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
PlayerData, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	PlayerData, 1, ReplayChannel, BLACK, 30.0f, BLACK, 30.0f	
	PlayerData, 2, PlayerTitleSetting, BLACK, 30.0f, BLACK, 30.0f			
	PlayerData, 3, PlayerIconSetting, BLACK, 30.0f, BLACK, 30.0f			
	PlayerData, 4, LocalBattleLog, BLACK, 30.0f, BLACK, 30.0f			
	LocalBattleLog, 0, PlayerData, BLACK, 30.0f, BLACK, 30.0f			
	LocalBattleLog, 1, MainMenu, BLACK, 30.0f, BLACK, 30.0f	
	ReplayChannel, 0, PlayerData, BLACK, 30.0f, BLACK, 30.0f	
	ReplayChannel, 1, MainMenu, BLACK, 30.0f, BLACK, 30.0f	
	TrialMode, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	RivalBattleMode, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	BossBattleMode, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	BonusStageMode1, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	BonusStageMode2, 0, MainMenu, BLACK, 30.0f, BLACK, 30.0f			
	Signout, 0, Title, BLACK, 30.0f, BLACK, 30.0f			
	StorageNotice, 0, Title, BLACK, 30.0f, BLACK, 30.0f		
)";

bool fVsStageSelect::forceTimerOnNextStageSelect = false;

void fGameEvents::Install() {
	MainMenu::Install();
	RootEvent::Install();
	VsBattle::Install();
	VsPreBattle::Install();
	VsStageSelect::Install();
}

void fMainMenu::Install() {
	int (fMainMenu:: * _fGetItemObserverState)() = &GetItemObserverState;
	void (fMainMenu:: * _fOnModeSelected)(int) = &OnModeSelected;
	DetourAttach((PVOID*)&rMainMenu::itemObserverMethods.GetItemObserverState, *(PVOID*)&_fGetItemObserverState);
	DetourAttach((PVOID*)&rMainMenu::itemObserverMethods.OnModeSelected, *(PVOID*)&_fOnModeSelected);
}


int fMainMenu::GetItemObserverState() {
	if (bOverrideItemObserverState != -1) {
		return bOverrideItemObserverState;
	}

	return (this->*rMainMenu::itemObserverMethods.GetItemObserverState)();
}

void fMainMenu::OnModeSelected(int mode) {
	if (OnModeSelectedOverride(mode)) {
		return;
	}

	return (this->*rMainMenu::itemObserverMethods.OnModeSelected)(mode);
}

void fRootEvent::Install() {
	*rRootEvent::eventFlowDefinition = fRootEvent::eventFlowDescription;
}

void fVsBattle::Install() {
	int (fVsBattle:: * _fCheckAndMaybeExitBasedOnBattleType)() = &CheckAndMaybeExitBasedOnExitType;
	int (fVsBattle:: * _fHasInitialized)() = &HasInitialized;
	void (fVsBattle:: * _fPrepareBattleRequest)() = &PrepareBattleRequest;
	void (fVsBattle:: * _fRegisterTasks)() = &RegisterTasks;
	void (fVsBattle:: * _fExitForeground)() = &ExitForeground;

	DetourAttach(
		(PVOID*)&rVsBattle::privateMethods.CheckAndMaybeExitBasedOnExitType,
		*(PVOID*)&_fCheckAndMaybeExitBasedOnBattleType
	);
	DetourAttach(
		(PVOID*)&rVsBattle::privateMethods.PrepareBattleRequest,
		*(PVOID*)&_fPrepareBattleRequest
	);
	DetourAttach(
		(PVOID*)&rVsBattle::publicMethods.HasInitialized,
		*(PVOID*)&_fHasInitialized
	);
	DetourAttach(
		(PVOID*)&rVsBattle::publicMethods.RegisterTasks,
		*(PVOID*)&_fRegisterTasks
	);
	DetourAttach(
		(PVOID*)&rVsBattle::publicMethods.ExitForeground,
		*(PVOID*)&_fExitForeground
	);

	DWORD dwOld = 0;
	if (VirtualProtect(
		rVsBattle::vt_IsTerminationComplete,
		sizeof(void*),
		PAGE_EXECUTE_READWRITE,
		&dwOld
	)) {
		*rVsBattle::vt_IsTerminationComplete = (BOOL(rVsBattle::*)()) & IsTerminationComplete;
		VirtualProtect(rVsBattle::vt_IsTerminationComplete, sizeof(void*), dwOld, &dwOld);
	}
	else {
		MessageBoxA(NULL, "Could not install VsBattle IsTerminationComplete override! Will crash!", NULL, MB_OK);
		DWORD error = GetLastError();
		DebugBreak();
	}
}


int fVsBattle::CheckAndMaybeExitBasedOnExitType() {
	if (bTerminateOnNextLeftBattle) {
		bTerminateOnNextLeftBattle = false;
		EventController* c = *EventBase::GetSourceController(this);
		(c->*EventController::publicMethods.EnterTerminalState)(0, 0);
		return 1;
	}

	return (this->*rVsBattle::privateMethods.CheckAndMaybeExitBasedOnExitType)();
}

int fVsBattle::HasInitialized() {
	if (!(this->*rVsBattle::publicMethods.HasInitialized)()) {
		// The real system hasn't initialized yet.
		return 0;
	}

	// The real system has initialized, but we may want to intentionally
	// delay.
	if (bBlockInitialization) {
		return 0;
	}
	if (fUserApp::netplay) {
		if (!bSessionSentLoaded) {
			bSessionSentLoaded = true;
			fUserApp::netplay->client.Battle_Loaded();
		}
		if (!bSessionSynced) {
			return 0;
		}
	}

	return 1;
}

BOOL fVsBattle::IsTerminationComplete() {
	rVsBattle* _this = (rVsBattle*)this;
	if (!(_this->*EventBase::publicMethods.IsTerminationComplete)()) {
		// The real system hasn't terminated yet.
		return 0;
	}

	// The real system has terminated, but we may want to intentionally
	// delay.
	return !bBlockTermination;
}

void fVsBattle::PrepareBattleRequest() {
	(this->*rVsBattle::privateMethods.PrepareBattleRequest)();
	Request* r = *rVsBattle::GetRequest(this);
	if (r) {
		if (bForceNextMatchOnline) {
			(r->*Request::publicMethods.SetIsOnlineBattle)(TRUE);
		}
		if (bOverrideNextRandomSeed) {
			(r->*Request::publicMethods.SetRandomSeed)(nextMatchRandomSeed);
		}
	}
	bForceNextMatchOnline = false;
	bOverrideNextRandomSeed = false;
	nextMatchRandomSeed = 0xffffffff;
}

void fVsBattle::RegisterTasks() {
	rVsBattle* _this = (rVsBattle*)this;
	(_this->*rVsBattle::publicMethods.RegisterTasks)();
	if (OnTasksRegistered) {
		OnTasksRegistered();
		OnTasksRegistered = nullptr;
	}
}

void fVsBattle::ExitForeground() {
	rVsBattle* _this = (rVsBattle*)this;
	(_this->*rVsBattle::publicMethods.ExitForeground)();
	if (fUserApp::netplay) {
		fUserApp::ResetLobbyForRematch();
	}
	bSessionSentLoaded = false;
	bSessionSynced = false;
}

void fVsPreBattle::Install() {
	void (fVsPreBattle:: * _fRegisterTasks)() = &RegisterTasks;
	DetourAttach((PVOID*)&fVsPreBattle::publicMethods.RegisterTasks, *(PVOID*)&_fRegisterTasks);
}

void fVsPreBattle::RegisterTasks() {
	rVsPreBattle* _this = (rVsPreBattle*)this;
	if (bSkipToVersus) {
		sf4e::Event::EventController::ReplaceNextEvent("VersusFromChr");
		bSkipToVersus = false;
	}
	(_this->*rVsPreBattle::publicMethods.RegisterTasks)();
	if (OnTasksRegistered) {
		OnTasksRegistered();
		OnTasksRegistered = nullptr;
	}
}

void fVsStageSelect::Install() {
	DetourAttach((PVOID*)&rVsStageSelect::staticMethods.Factory, &Factory);
}

rVsStageSelect* fVsStageSelect::Factory(DWORD arg1, DWORD arg2, DWORD arg3) {
	rVsStageSelect* out = rVsStageSelect::staticMethods.Factory(arg1, arg2, arg3);
	if (forceTimerOnNextStageSelect) {
		rVsStageSelect::GetState(out)->flags |= StageSelectState::SSSF_TIMER_ENABLED;
	}
	return out;
}
