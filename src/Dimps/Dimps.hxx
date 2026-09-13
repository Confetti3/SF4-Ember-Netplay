#pragma once

#include <windows.h>

#include "Dimps__GameEvents.hxx"

namespace Dimps {
	extern char** characterCodes;
	extern char** characterNames;
	extern char** stageCodes;
	extern char** stageNames;

	void Locate(HMODULE peRoot);

	struct App {
		static void Locate(HMODULE peRoot);
		static GameEvents::RootEvent* (*GetRootEvent)();
	};
}
