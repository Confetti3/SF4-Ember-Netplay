#pragma once

#include <windows.h>

#include "Dimps__Eva.hxx"
#include "Dimps__Game.hxx"

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Camera {
				void Locate(HMODULE peRoot);

				struct Unit
				{
					typedef struct __privateMethods {
						// TODO
					} __privateMethods;

					typedef struct __publicMethods {
						void (Unit::* RecordToInternalMementoKey)(Dimps::Game::GameMementoKey::MementoID* id);
						void (Unit::* RestoreFromInternalMementoKey)(Dimps::Game::GameMementoKey::MementoID* id);
					} __publicMethods;

					typedef struct __staticMethods {
						// TODO
					} __staticMethods;

					static Eva::Task** GetCamShakeTask(Unit* u);

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