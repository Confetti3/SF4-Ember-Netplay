#pragma once

#include <windows.h>
#include <string>

#include "../Dimps/Dimps__Game__Battle__Vfx.hxx"
#include "../Dimps/Dimps__Platform.hxx"

namespace sf4e {
	namespace Game {
		namespace Battle {
			namespace Vfx {
				void Install();

				struct ColorFade : Dimps::Game::Battle::Vfx::ColorFade
				{

					Dimps::Platform::list_entry<Dimps::Game::Battle::Vfx::ColorFadeData>* Spawn(void* sourceData);

					static int HIGHEST_OBSERVED_FADES;
					static void Install();
				};
			}
		}
	}
}