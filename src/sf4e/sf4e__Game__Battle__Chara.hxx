#pragma once

#include "../Dimps/Dimps__Game.hxx"

namespace sf4e {
	namespace Game {
		namespace Battle {
			namespace Chara {
				void Install();

				// Detours for Dimps::Game::Battle::Chara::Afterimage's memento
				// interface; `this` is its IMementoable subobject (object +0x60).
				struct Afterimage {
					static void Install();

					size_t GetMementoSize();
					int RecordToMemento(void* memento, Dimps::Game::GameMementoKey::MementoID* id);
					int RestoreFromMemento(void* memento, Dimps::Game::GameMementoKey::MementoID* id);
				};
			}
		}
	}
}
