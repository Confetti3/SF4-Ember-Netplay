#pragma once

#include <random>
#include <string>
#include <windows.h>

#include "../Dimps/Dimps__Eva.hxx"
#include "../common/sf4e__NetplayConfig.hxx"
#include "../platform/HelperProcess.hxx"

namespace sf4e {
	typedef struct Args {
		bool bShowConsole = false;
	} Args;

	typedef struct Payload {
		uint32_t magic = 0x53463442; // SF4 bootstrap, independent of session settings.
		uint32_t version = 1;
		Args args;
		HANDLE hSyncEvent = NULL;
		platform::HelperBootstrap helper;
        platform::HelperBootstrap discord;
		uint32_t helperError = 0;
		NetplayConfig netplay = {};
	} Payload;

	inline bool IsCompatiblePayload(const Payload* payload, size_t length) {
		return payload && length == sizeof(Payload) && payload->magic == 0x53463442 && payload->version == 1 &&
			payload->netplay.version == SF4E_NETPLAY_CONFIG_VERSION;
	}

	extern std::string sidecarHash;
	extern std::mt19937 localRand;
	extern Args args;
	extern HANDLE hSyncEvent;

	void Install(HINSTANCE hinstDll, const Payload* const payload);

	namespace Eva {
		struct IEmSpriteAction : Dimps::Eva::IEmSpriteAction {
			struct AdditionalMemento {
				Dimps::Eva::IEmSpriteAction action;
			};

			static void RecordToAdditionalMemento(Dimps::Eva::IEmSpriteAction* a, AdditionalMemento& m);
			static void RestoreFromAdditionalMemento(Dimps::Eva::IEmSpriteAction* a, const AdditionalMemento& m);
		};

		struct Task : Dimps::Eva::Task {
			struct TaskFunctorBuf {
				char pad[0x10];
			};

			struct AdditionalMemento {
				Dimps::Eva::Task rawTask;

				TaskFunctorBuf cancelFunctor;
				TaskFunctorBuf workFunctor;
				bool hasCancelFunctor;
				bool hasWorkFunctor;
			};

			// False when a functor has an unknown or oversized vtable; that
			// functor is left out and the state must not be used.
			static bool RecordToAdditionalMemento(Dimps::Eva::Task* t, AdditionalMemento& m);
			static void RestoreFromAdditionalMemento(Dimps::Eva::Task* t, const AdditionalMemento& m);
		};

		struct TaskCore : Dimps::Eva::TaskCore {
			// This is wrong- this is variable length, but we only care about storing
			// the data of the System task core right now. It would make more sense
			// for this to be associated with the Task memento, but the core is the
			// object that knows how large the private per-task data is.
			struct TaskDataBuf {
				// Derived from 0x5da300
				char pad[0x20];
			};

			struct AdditionalMemento {
				int numUsed;
				Task::AdditionalMemento tasks[MAX_TASKS_PER_CORE];
				TaskDataBuf taskdata[MAX_TASKS_PER_CORE];
			};

			// Records what fits and sets recordFailed when the core holds state
			// the memento cannot represent; restore sets restoreFailed when it
			// cannot rebuild a task. The game's memento chain has no error
			// path, so only SaveState::Save and SaveState::Load reset and read
			// these flags, and report them through their return values.
			static void RecordToAdditionalMemento(Dimps::Eva::TaskCore* c, AdditionalMemento& m);
			static void RestoreFromAdditionalMemento(Dimps::Eva::TaskCore* c, const AdditionalMemento& m);
			static bool recordFailed;
			static bool restoreFailed;
		};
	}
}
