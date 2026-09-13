#pragma once

#include "../Dimps/Dimps__Game__Battle__Effect.hxx"

namespace sf4e {
	namespace Game {
		namespace Battle {
			namespace Effect {
				void Install();

				struct ActorManager : Dimps::Game::Battle::Effect::ActorManager {
					size_t GetMementoSize();

					static void Install();
				};
			}
		}
	}
}