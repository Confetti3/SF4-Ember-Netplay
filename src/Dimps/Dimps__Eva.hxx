#pragma once

#include <cstddef>

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

		// One of the effect engine's object pools. initPools (0x6C4B8D..0x6C4BAA)
		// carves it, EmEffectElementPool::alloc (0x7161A0) pops freeStack[used]
		// and raises used and peak (0x7161D8..0x7161F0), or returns NULL once used
		// reaches capacity (0x7161BB); its free (0x716210) lowers used again.
		struct EmEffectPool {
			void* heapBlock;
			void* elements;
			void** freeStack;
			DWORD capacity;
			DWORD used;
			// The most ever in use, from boot until shutdown, not per battle: only
			// EmEffectManager::reset (0x6C4F20) and freePools (0x6C47D0, through
			// 0x6C4FB0, which also zeroes capacity and used) clear it.
			DWORD peak;
			DWORD pad_0x18;
			DWORD pad_0x1c;
		};
		static_assert(sizeof(EmEffectPool) == 0x20, "an effect pool is 32 bytes");
		static_assert(offsetof(EmEffectPool, capacity) == 0xc && offsetof(EmEffectPool, used) == 0x10 &&
			offsetof(EmEffectPool, peak) == 0x14, "an effect pool's counts");

		// The effect engine, one static object (0xAADE80) that getInstance
		// (0x6C4EB0) builds on first use and never returns null for. EvaApp::onInit
		// (0x4053CF) makes its pools with capacities 256, 1024 and 2048 and
		// EvaApp::onShutdown (0x40542A) frees them, so they live for the process.
		struct EmEffectManager {
			enum PoolKind {
				PK_EFFECT,  // 0xF0-byte effect instances (createEffect 0x6C4CD8)
				PK_NODE,    // 0x50-byte nodes (EmEffectPools::freeNode 0x715850)
				// 0x120-byte EmEffectElementEx. A rollback restore takes them in
				// EmEffectEmitterNode::loadState (0x74D930), which reads the result
				// of alloc unchecked at 0x74DBBE.
				PK_ELEMENT,
				PK_COUNT
			};

			void* vtbl;
			DWORD poolCounts[PK_COUNT]; // the capacities initPools was given (0x6C4BB6)
			EmEffectPool pools[PK_COUNT];

			// The object itself, read as data: getInstance would run its lazy
			// constructor (0x6C4900) if called before the game does.
			typedef struct __staticVars {
				EmEffectManager* Instance;
			} __staticVars;

			static void Locate(HMODULE peRoot);
			static __staticVars staticVars;
		};
		static_assert(offsetof(EmEffectManager, pools) == 0x10, "the effect pools follow their counts");

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

			static DWORD* Get0x8(Task* t) {
				return (DWORD*)((unsigned int)t + 0x8);
			}
			static DWORD* GetFlags(Task* t) {
				return (DWORD*)((unsigned int)t + 0x4);
			}
			static Task** GetNext(Task* t) {
				return (Task**)((unsigned int)t + 0x20);
			}
			static Task** GetPrevious(Task* t) {
				return (Task**)((unsigned int)t + 0x1c);
			}
			static DWORD* GetPhase(Task* t) {
				return (DWORD*)((unsigned int)t + 0x2c);
			}
			static int* GetPriority(Task* t) {
				return (int*)((unsigned int)t + 0xc);
			}
			static DWORD* GetState(Task* t);
			static void** GetTaskData(Task* t) {
				return (void**)((unsigned int)t + 0x30);
			}
			static IEmTaskFunctor** GetCancelFunctor(Task* t) {
				return (IEmTaskFunctor**)((unsigned int)t + 0x18);
			}
			static IEmTaskFunctor** GetWorkFunctor(Task* t) {
				return (IEmTaskFunctor**)((unsigned int)t + 0x14);
			}
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
			static Task** GetTaskBuffer(TaskCore* c) {
				return *(Task***)((unsigned int)c + 0x38);
			}
			static size_t* GetTaskDataSize(TaskCore* c) {
				return (size_t*)((unsigned int)c + 0x24);
			}
			static Task* GetTaskHead(TaskCore* c) {
				return *(Task**)((unsigned int)c + 0x48);
			}
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