#pragma once

#include <windows.h>
#include <atomic>

#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Platform.hxx"

namespace sf4e {
	namespace GameEvents {
		void Install();

		struct MainMenu : Dimps::GameEvents::MainMenu
		{
			void* Destroy(DWORD arg1);
			void OnModeSelected(int mode);
			int GetItemObserverState();

			// On the item observer (ToItemObserver), like GoToVersusMode.
			void GoToLocalBattleLog();
			static bool OpenLocalBattleLog();
			// Leaves the battle log for the main menu (its flow row 1), the way
			// its own Back leaves for Player Data (row 0, 0x46F910).
			static bool LeaveLocalBattleLog();
			static int (*OnModeSelectedOverride)(int mode);
			// Written by the overlay on the drawing thread, read by the game.
			static std::atomic<int> bOverrideItemObserverState;
			static void Install();
		};

		struct RootEvent : Dimps::GameEvents::RootEvent
		{
			static char* eventFlowDescription;
			static void Install();
		};

		struct VsBattle : Dimps::GameEvents::VsBattle
		{
			int CheckAndMaybeExitBasedOnExitType();
			int HasInitialized();
			BOOL IsTerminationComplete();
			void PrepareBattleRequest();
			void RegisterTasks();
			void ExitForeground();

			static void (*OnTasksRegistered)();
			static bool bSessionSentLoaded;
			static bool bSessionSynced;
			static bool bBlockInitialization;
			static bool bBlockTermination;
			static bool bForceNextMatchOnline;
			static bool bOverrideNextRandomSeed;
			static bool bTerminateOnNextLeftBattle;
			static DWORD nextMatchRandomSeed;
			static void Install();
		};

		struct VsPreBattle : Dimps::GameEvents::VsPreBattle
		{
			static void (*OnTasksRegistered)();
			static bool bSkipToVersus;

			void RegisterTasks();
			static void Install();
		};

		struct VsStageSelect : Dimps::GameEvents::VsStageSelect
		{
			static bool forceTimerOnNextStageSelect;
			static Dimps::GameEvents::VsStageSelect* Factory(DWORD arg1, DWORD arg2, DWORD arg3);
			static void Install();
		};
	}
}