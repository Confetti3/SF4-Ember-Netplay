#pragma once

#include <windows.h>

#include "Dimps__Math.hxx"

#define NUM_ACTION_STATES 8
#define MAX_TASKS_PER_CORE 0x400

namespace Dimps {
	namespace Eva {
		using Dimps::Math::FixedPoint;
		using Dimps::Math::Matrix4x4;

		void Locate(HMODULE peRoot);

		struct EmRandom {
			void* vtbl;
			DWORD seed;
		};

		struct IEmSpriteAction {
			char pad[0x260];

			struct ActionState {
				void* pad_0x0;
				DWORD pad_0x4;
				FixedPoint currentFrame;
				FixedPoint endFrame;
				FixedPoint timescale;
				DWORD timer_0x14;
				DWORD pad_0x18;
				DWORD active_0x1c;
				DWORD loop_0x20;
			};

			typedef struct __publicMethods {
				ActionState* (IEmSpriteAction::* GetActionState)(int idx);
			} __publicMethods;

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
		};

		struct IEmSpriteNode {
			void* vtbl;
			void* emsData;
			DWORD pad_0x8;
			DWORD flags;
			IEmSpriteNode* firstChild;
			IEmSpriteNode* nextSibling;
			IEmSpriteNode* prevSibling;
			IEmSpriteAction* action;
			Matrix4x4 mat;

			typedef struct __publicMethods {
				int (IEmSpriteNode::* GetNumChildren)();
				IEmSpriteNode* (IEmSpriteNode::* GetChildByIdx)(int idx);
				IEmSpriteAction* (IEmSpriteNode::* GetAction)();
			} __publicMethods;

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
		};

		struct IEmTaskFunctor {
			// This location makes very little sense, but no location makes
			// sense for this- descendant functors do not expose how large
			// they are, so pointers to functors can't ever be copied.
			static size_t GetDescendantFunctorSizeByVtable(DWORD functorVtable);
			static void** GetImpl(IEmTaskFunctor* impl);
		};

		struct Task {
			char pad[0x60];

			enum TaskState {
				TS_EMPTY = 0x0,
				TS_WORKING = 0x1,
				TS_CANCELLED = 0x2,
				TS_UNK3 = 0x3
			};

			static DWORD* Get0x8(Task* t);
			static DWORD* GetFlags(Task* t);
			static Task** GetNext(Task* t);
			static Task** GetPrevious(Task* t);
			static DWORD* GetPhase(Task* t);
			static int* GetPriority(Task* t);
			static DWORD* GetState(Task* t);
			static void** GetTaskData(Task* t);
			static IEmTaskFunctor** GetCancelFunctor(Task* t);
			static IEmTaskFunctor** GetWorkFunctor(Task* t);
		};

		struct TaskCore {
			typedef struct __publicMethods {
				Task** (TaskCore::* AllocateNewTask)(Task** out, unsigned int priority, DWORD flags, DWORD arg4);
				void (TaskCore::* CancelTask)(Task** t);
				char* (TaskCore::* GetName)();
				int (TaskCore::* GetNumUsed)();
				char* (TaskCore::* GetTaskName)(Task** t);
				void (TaskCore::* ReclaimCancelledTasks)();
			} __publicMethods;

			static void Locate(HMODULE peRoot);
			static Task** GetTaskBuffer(TaskCore* c);
			static size_t* GetTaskDataSize(TaskCore* c);
			static Task* GetTaskHead(TaskCore* c);
			static __publicMethods publicMethods;
		};

		struct TaskCoreRegistry {
			typedef struct __staticMethods {
				TaskCore* (*GetCoreByIndex)(int index);
				int (*GetNumActiveCores)();
			} __staticMethods;

			static void Locate(HMODULE peRoot);
			static __staticMethods staticMethods;
		};
	}
}