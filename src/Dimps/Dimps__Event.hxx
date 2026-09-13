#pragma once

#include <windows.h>

namespace Dimps {
	namespace Event {
		void Locate(HMODULE peRoot);

		// These structs are heavily complected- provide forward declarations
		// so that the full struct definitions can be in arbitrary order.
		struct EventBase;
		struct EventController;

		struct EventBase {
			static char* GetName(EventBase* e);
			static EventController** GetSourceController(EventBase* e);
			static void Locate(HMODULE peRoot);

			typedef struct __publicMethods {
				// 17 different children of EventBase (most Battle-related events)
				// have identical implementations of their wait-for-exit method,
				// and their only shared ancestor is EventBase. Since the
				// implementations are identical, the compiler deduplicated the
				// implementations, which makes it impossible to safely represent
				// the original source implementation with pointers in each
				// battle-event class. Instead, _one_ pointer to the deduplicated
				// implementation is stored here, in the nearest parent.
				BOOL(EventBase::* IsTerminationComplete)();
			} __publicMethods;

			static __publicMethods publicMethods;
		};

		struct EventBaseWithEC : EventBase {
			static EventBase* FindForegroundEvent(EventBaseWithEC* start, char** eventNames, int numEvents);
			static void Locate(HMODULE peRoot);

			typedef struct __publicMethods {
				EventController* (EventBaseWithEC::* GetChildEventController)();
			} __publicMethods;

			static __publicMethods publicMethods;
		};

		struct EventController {
			static void Locate(HMODULE peRoot);

			enum EventQueueStatus {
				EQS_EMPTY = 0,
				EQS_WAITING_FOR_EMPTY = 1,
				EQS_INITIALIZATION_QUEUED = 2,
				EQS_INITIALIZING = 3,
				EQS_INITIALIZED = 4,
				EQS_WAITING_FOR_FOREGROUND = 5,
				EQS_FOREGROUND = 6,
				EQS_EXITING_FOREGROUND = 7,
				EQS_FINALIZATION_QUEUED = 8,

				// References to this exist in the codebase, but the presence of
				// it is either a potential bug or an incomplete feature- finalized
				// event instances are marked as empty for reuse by the event
				// controller.
				EQS_FINALIZED__UNUSED = 9,
			};

			typedef struct __publicMethods {
				void (EventController::* EnterTerminalState)(DWORD arg1, DWORD arg2);
				EventBase* (EventController::* GetForegroundEvent)();
				int (EventController::* GetForegroundEventStatus)();
				void (EventController::* QueueEvent)(char* eventName, DWORD arg1, DWORD arg2, DWORD bWaitForEmpty);
				void (EventController::* RunUpdate)();
			} __publicMethods;

			static __publicMethods publicMethods;
		};
	}
}