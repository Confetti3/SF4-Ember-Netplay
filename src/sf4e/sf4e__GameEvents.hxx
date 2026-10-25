#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>
#include <optional>

#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../common/NextBattleRequest.hxx"
#include "../netplay/SessionController.hxx"

namespace sf4e {
	namespace NetplayFacade { struct RuntimeSnapshot; }

	namespace GameEvents {
		void Install();
        struct OptionUIController : Dimps::GameEvents::OptionUIController {
            void* Construct(DWORD owner, DWORD mode);
            int Destroy();
            static void Install();
        };

		// What Ember's Training select chose: both sides as the game takes them,
		// and the stage (a custom stage plays on its fallback, as in rooms).
		struct TrainingSides {
			Dimps::GameEvents::VsMode::ConfirmedCharaConditions sides[2] = {};
			int stage = 0;
		};

		// The one pending request to leave for Training, whole under one lock: the
		// runtime posts it, the native menu reads, judges and consumes it. A
		// consume names the serial it judged, so it never erases a newer request.
		class TrainingRequest {
		public:
			struct Pending { netplay::Generation generation; ULONGLONG deadline = 0; unsigned long long serial = 0; };
			// Replaces any pending request; returns its serial.
			unsigned long long Post(const netplay::Generation& generation, ULONGLONG now, ULONGLONG lifetime) {
				std::lock_guard<std::mutex> guard(lock_);
				pending_ = Pending{ generation, now + lifetime, ++serial_ };
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
			// The pending request judged now by `holds(generation)`, and
			// forgotten either way: Go when it still holds and is the caller's to act
			// on, Dropped when it no longer does, None when there is none or a newer
			// one replaced it meanwhile (that one is left alone).
			enum class Taken { None, Dropped, Go };
			template<class Holds> Taken Take(ULONGLONG now, const Holds& holds) {
				Pending request;
				if (!Peek(now, request)) return Taken::None;
				const bool go = holds(request.generation);
				if (!Consume(request.serial)) return Taken::None;
				return go ? Taken::Go : Taken::Dropped;
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
			// On the event, not its item observer.
			void IdleTask(int arg);

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
			// Fight Request question. Posted by the runtime once it has accepted
			// a room's TrainingEntry; the game thread acts on it the next time the
			// menu is idle, and forgets it after two seconds, so a request made where
			// no main menu is up does nothing later.
			// `generation` is the session the request was accepted in.
			// Just before it moves, the menu asks the runtime whether the request
			// still holds (NetplayFacade::TrainingRequestHolds) and forgets it
			// otherwise; the runtime owns that policy.
			static void RequestTraining(const netplay::Generation& generation);
			static TrainingRequest trainingRequest;
			static void Install();
		};

		struct RootEvent : Dimps::GameEvents::RootEvent
		{
			static char* eventFlowDescription;
			static void Install();
		};

		// Training on Training select's picks: chosen ones the idle main menu
		// enters within two seconds, with no room or match open; a room's Training
		// has none and plays on the Training stage.
		struct TrainingMode {
			// The overlay, on Start.
			static void Choose(const TrainingSides& sides);
			static bool Chosen();
			// Game thread, entering Training: sets the stage and takes the chosen sides.
			static void Enter();
			// Game thread: with sides taken, Training's CharaSelect becomes
			// VersusFromChr on them, once.
			static void ReplaceCharaSelect(char*& eventName);
		private:
			static std::mutex chosenLock;
			static std::optional<TrainingSides> chosen;
			static ULONGLONG chosenUntil;
			// Game thread only.
			static std::optional<TrainingSides> pendingSides;
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
			static bool bTerminateOnNextLeftBattle;
			// The seed and Training rules PrepareBattleRequest gives the
			// next battle, taken there once (battle::NextRequest).
			static sf4e::battle::NextRequest nextRequest;
			static void Install();
		};

		struct VsPreBattle : Dimps::GameEvents::VsPreBattle
		{
			static void (*OnTasksRegistered)();
			static bool bSkipToVersus;
			// The PVP battle settings a Versus battle from the main menu gets;
			// no recordReplay keeps the game's own. next and
			// onBattleTasksRegistered become VsBattle's nextRequest and
			// OnTasksRegistered for that battle.
			struct VersusSettings {
				BOOL editionSelect = FALSE;
				int rounds = 1;
				Dimps::Math::FixedPoint timeLimit{};
				std::optional<BOOL> recordReplay;
				sf4e::battle::NextRequest next;
				void (*onBattleTasksRegistered)() = nullptr;
			};
			// A Versus battle from the main menu with no select screens, as a
			// room's match is entered: the settings go into the PVP battle type,
			// chosen for the next battle, the battle's request and callback are
			// armed, and VS mode skips to the battle and calls onTasksRegistered
			// once its tasks are registered, where SetSides gives it the fighters
			// and the stage. False, with nothing written, when the main menu is
			// not in front.
			static bool GoToVersus(const VersusSettings& settings, void (*onTasksRegistered)());
			// False when VS mode is not in front.
			static bool SetSides(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions (&sides)[2], int stage);

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