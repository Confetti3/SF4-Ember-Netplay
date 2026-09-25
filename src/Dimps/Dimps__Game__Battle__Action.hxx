#pragma once

#include <windows.h>

#include "Dimps__Game.hxx"

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Action {
				void Locate(HMODULE peRoot);

				// Called through the IMementoable subobject at +0x60, the `this`
				// every memento entry point receives. Record and restore return
				// nonzero, or 0 for a null memento; restore also rebuilds the
				// five collision-box lists and restores the action engine at
				// *(this+80) by the same memento id.
				struct Actor
				{
					typedef struct __publicMethods {
						size_t (Actor::* GetMementoSize)();
						int (Actor::* RecordToMemento)(void* memento, GameMementoKey::MementoID* id);
						int (Actor::* RestoreFromMemento)(void* memento, GameMementoKey::MementoID* id);
					} __publicMethods;

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;
				};
			}
		}
	}
}