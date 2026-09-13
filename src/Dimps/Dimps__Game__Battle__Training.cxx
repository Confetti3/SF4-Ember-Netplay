#include <windows.h>

#include "Dimps__Game__Battle__Training.hxx"

namespace Training = Dimps::Game::Battle::Training;
using Training::Manager;

Manager::__privateMethods Manager::privateMethods;
Manager::__publicMethods Manager::publicMethods;
Manager::__staticMethods Manager::staticMethods;

void Training::Locate(HMODULE peRoot) {
	Manager::Locate(peRoot);
}

void Manager::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.RecordToInternalMementoKey = (PVOID)(peRootOffset + 0x1e2770);
	*(PVOID*)&publicMethods.RestoreFromInternalMementoKey = (PVOID)(peRootOffset + 0x1e25a0);

	staticMethods.GetSingleton = (Manager * (*)())(peRootOffset + 0x1e28b0);
}