#pragma once

#include <windows.h>

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Action {
				void Locate(HMODULE peRoot);

				struct Actor
				{
					typedef struct __publicMethods {
						size_t (Actor::* GetMementoSize)();
					} __publicMethods;

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;
				};
			}
		}
	}
}