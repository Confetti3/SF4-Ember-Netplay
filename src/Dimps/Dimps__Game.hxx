#pragma once

#include <windows.h>

#include "Dimps__Eva.hxx"

namespace Dimps {
	namespace Game {
		using Dimps::Eva::IEmSpriteNode;

		void Locate(HMODULE peRoot);

		struct GameMementoKey {
			struct MementoID {
				DWORD lo;
				DWORD hi;
			};

			struct Metadata {
				MementoID id;
				void* memento;
			};

			typedef struct __publicMethods {
				void (GameMementoKey::* Initialize)(void* mementoable, int numMementos);
				void (GameMementoKey::* ClearKey)();
			} __publicMethods;

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;

			static int* totalMementoSize;

			void* mementoableObject;
			Metadata* metadata;
			int numMementos;
			void* mementos;
			int sizeAllocated;
			int nextMementoIndex;
		};

		struct ProgressData {
			enum NextBattleType {
				NBT_PVP = 0,
				NBT_PVC = 1,
				NBT_CVC = 2,
			};

			struct BattleTypeSettings {
				int diffculty;
				int rounds;
				Math::FixedPoint timeLimit;
				BOOL recordReplay;
				BOOL editionSelect;
			};

			static NextBattleType* GetNextBattleType(ProgressData* data);
			static BattleTypeSettings* GetBattleTypeSettings(ProgressData* data);
		};

		struct Request {
			typedef struct __publicMethods {
				DWORD (Request::* GetRandomSeed)();
				void (Request::* SetIsOnlineBattle)(BOOL isOnlineBattle);
				void (Request::* SetRandomSeed)(DWORD seed);
				// One of a player's battle parameters. The request keeps a
				// table of them a player (0x2B of them, at +0xF0, 0x2B8 bytes
				// a player) and refuses a value outside the parameter's own
				// range. The battle reads them back through
				// Battle::System's vtable +0x158.
				void (Request::* SetPlayerParam)(int player, int param, int value);
			} __publicMethods;

			// The parameters the fighter's own update reads every frame for
			// its gauges, and what their values do there (actor code at
			// 0x5529F0, 0x552B20, 0x557E20, 0x557F60, 0x552E50):
			// PM_NORMAL the gauge behaves as in a match, PM_EMPTY holds it
			// at nothing, PM_FULL holds it full, PM_REFILL fills it again
			// while the fighter is left alone. Training mode sets the same
			// values from its pause menu. PP_VITALITY at PM_REFILL also
			// skips the knockout (0x55C89A).
			enum PlayerParam {
				PP_VITALITY = 0x1A,
				PP_RECOVERABLE_VITALITY = 0x1C,
				PP_SUPER_COMBO = 0x1E,
				PP_REVENGE = 0x21,
				PP_STUN = 0x24,
			};
			enum ParamMode { PM_NORMAL = 0, PM_EMPTY = 6, PM_FULL = 7, PM_REFILL = 8 };

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
		};

		namespace Sprite {
			struct Control {
				typedef struct __publicMethods {
					// The polymorphism here is technically wrong- these methods
					// are deduplicated by the compiler for subclasses of Control
					// with identical implementations. There's no way to safely
					// express this duplication with Detours, because multiple
					// pointers to the same function could hypothetically
					// be independently detoured, which would result in a last-writer-wins
					// situation. Instead, just put it in the parent class and be careful.
					void(Control::* Disable_0x57bd80)();
					void(Control::* Enable_0x577910)();
					void(Control::* Enable_0x588450)();
				} __publicMethods;

				static DWORD* GetEnabled(Control* c);
				static void Locate(HMODULE peRoot);
				static __publicMethods publicMethods;
			};

			struct SingleNodeControl : Control {
				static int* GetCurrentFrame(SingleNodeControl* c);
				static IEmSpriteNode** GetSpriteNode(SingleNodeControl* c);
			};
		}
	}
}