#pragma once

#include <windows.h>

#include "Dimps__Eva.hxx"
#include "Dimps__Game.hxx"

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Training {
				void Locate(HMODULE peRoot);

				struct Manager
				{
					typedef struct __privateMethods {
						// TODO
					} __privateMethods;

					typedef struct __publicMethods {
						void (Manager::* RecordToInternalMementoKey)(Dimps::Game::GameMementoKey::MementoID* id);
						void (Manager::* RestoreFromInternalMementoKey)(Dimps::Game::GameMementoKey::MementoID* id);
					} __publicMethods;

					typedef struct __staticMethods {
						Manager* (*GetSingleton)();
					} __staticMethods;

					static void Locate(HMODULE peRoot);
					static __privateMethods privateMethods;
					static __publicMethods publicMethods;
					static __staticMethods staticMethods;

					// The pause menu's TRAINING OPTIONS, in menu order: 14 ints at
					// +0x8 that the virtual getter (vtable +0x10) and setter (+0x14)
					// index directly. The dummy driver and the hit code read them
					// through Battle::System every frame, so a write applies at
					// once. Values are the menu's choice indices, except the
					// gauges (0 normal, 5 max, 7 infinite, 8 refill).
					enum Option {
						// stand, crouch, jump, cpu; 4 and 5 are the menu's own recorder.
						OPT_ACTION = 0,
						// no block, after first hit, all, random
						OPT_GUARD = 1,
						// quick, normal, delayed, random
						OPT_QUICK_STAND = 2,
						// off, on, random
						OPT_COUNTER_HIT = 3,
						// normal, constant, none
						OPT_STUN = 4,
						OPT_SC_GAUGE = 6,
						OPT_REVENGE_GAUGE = 7,
						OPT_ATTACK_DATA = 8,
						OPT_INPUT_DISPLAY = 9,
						OPT_CPU_LEVEL = 10,
						OPTION_COUNT = 14
					};
					static int* GetOptions(Manager* m) { return (int*)((unsigned int)m + 0x8); }
				};
			}
		}
	}
}