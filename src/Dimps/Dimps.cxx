#include <windows.h>

#include "Dimps.hxx"
#include "Dimps__Eva.hxx"
#include "Dimps__Event.hxx"
#include "Dimps__Game.hxx"
#include "Dimps__GameEvents.hxx"
#include "Dimps__Pad.hxx"
#include "Dimps__Platform.hxx"
#include "Dimps__UserApp.hxx"
#include "Dimps__Selection.hxx"

char** Dimps::characterCodes;
char** Dimps::characterNames;
char** Dimps::stageCodes;
char** Dimps::stageNames;
Dimps::GameEvents::RootEvent* (*Dimps::App::GetRootEvent)();

void Dimps::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	characterCodes = (char**)(peRootOffset + 0x66a8a8);
	characterNames = (char**)(peRootOffset + 0x66a958);
	stageCodes = (char**)(peRootOffset + 0x66b678);
	stageNames = (char**)(peRootOffset + 0x66b600);

	App::Locate(peRoot);
	Eva::Locate(peRoot);
	Event::Locate(peRoot);
	Game::Locate(peRoot);
	GameEvents::Locate(peRoot);
	Pad::Locate(peRoot);
	Platform::Locate(peRoot);
	UserApp::Locate(peRoot);
	Selection::Locate(peRoot);
}

void Dimps::App::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	GetRootEvent = (GameEvents::RootEvent*(*)())(peRootOffset + 0x0299e0);
}
