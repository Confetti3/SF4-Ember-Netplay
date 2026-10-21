#pragma once

#include <windows.h>

#include "Dimps__Game.hxx"

using Dimps::Game::GameMementoKey;

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Effect {
				void Locate(HMODULE peRoot);

				struct ActorManager
				{
					typedef struct __publicMethods {
						size_t(ActorManager::* GetMementoSize)();
					} __publicMethods;

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;
				};

				struct Unit
				{
					typedef struct __privateMethods {
						// TODO
					} __privateMethods;

					typedef struct __publicMethods {
						void (Unit::* RecordToInternalMementoKey)(GameMementoKey::MementoID* id);
						void (Unit::* RestoreFromInternalMementoKey)(GameMementoKey::MementoID* id);
					} __publicMethods;

					typedef struct __staticMethods {
						// TODO
					} __staticMethods;

					// The first of a fighter's effect objects (a fireball), null
					// when it has none. Its boxes that hit are in list BOXES_HIT
					// (Chara::Actor::GetAttackBoxes).
					static void* GetFirstEffect(Unit* u, unsigned side);
					enum { BOXES_HIT = 3 };

					static void Locate(HMODULE peRoot);
					static __privateMethods privateMethods;
					static __publicMethods publicMethods;
					static __staticMethods staticMethods;

					// Instance values here
				};
			}
		}
	}
}