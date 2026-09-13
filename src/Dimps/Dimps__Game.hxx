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
			} __publicMethods;

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