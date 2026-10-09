#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>

#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../netplay/SessionController.hxx"

namespace sf4e {
	namespace NetplayFacade { struct RuntimeSnapshot; }

	namespace GameEvents {
		void Install();

		// The one pending request to leave for Training, whole under one lock: the
		// overlay posts it, the game thread reads, judges and consumes it. A
		// consume names the serial it judged, so it never erases a newer request.
		class TrainingRequest {
		public:
			struct Pending { netplay::Generation generation; bool fromRoom = false; ULONGLONG deadline = 0; unsigned long long serial = 0; };
			// Replaces any pending request; returns its serial.
			unsigned long long Post(const netplay::Generation& generation, bool fromRoom, ULONGLONG now, ULONGLONG lifetime) {
				std::lock_guard<std::mutex> guard(lock_);
				pending_ = Pending{ generation, fromRoom, now + lifetime, ++serial_ };
				active_ = true;
				return serial_;
			}
			// The pending request, or false when there is none or it has run out (which forgets it).
			bool Peek(ULONGLONG now, Pending& out) {
				std::lock_guard<std::mutex> guard(lock_);
				if (!active_) return false;
				if (now > pending_.deadline) { active_ = false; return false; }
				out = pending_;
				return true;
			}
			// Forgets the request with this serial. False, leaving everything alone, if it was replaced or already gone.
			bool Consume(unsigned long long serial) {
				std::lock_guard<std::mutex> guard(lock_);
				if (!active_ || pending_.serial != serial) return false;
				active_ = false;
				return true;
			}
		private:
			std::mutex lock_;
			Pending pending_;
			unsigned long long serial_ = 0;
			bool active_ = false;
		};

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
			// Asks the main menu to leave for Training mode without its
			// Fight Request question. Any thread; the game thread acts on it
			// the next time the menu is idle, and forgets it after two
			// seconds, so a request made where no main menu is up does
			// nothing later.
			// `generation` is the session the request was made in; `fromRoom` is
			// true for Training called from a room, false for the offline path.
			static void RequestTraining(const netplay::Generation& generation, bool fromRoom);
			// Whether a request still holds in the runtime's latest published state:
			// the session it was made in must still be current, and a room request
			// needs the room's own gate open; an offline one needs its StartOffline
			// accepted. Wait is "not yet", Drop is "no longer, forget it".
			enum class TrainingVerdict { Proceed, Wait, Drop };
			static TrainingVerdict JudgeTrainingRequest(const NetplayFacade::RuntimeSnapshot& live,
				const netplay::Generation& generation, bool fromRoom);
			static TrainingRequest trainingRequest;
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
				// The next battle is played by a table's Training rules:
				// both fighters' health and gauges fill again and nobody
				// is knocked out. Part of the simulation, so both sides
				// and every spectator of a match must set it alike.
				static bool bNextMatchTraining;
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