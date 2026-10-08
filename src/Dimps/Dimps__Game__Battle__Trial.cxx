#include <windows.h>

#include "Dimps__Game__Battle__Trial.hxx"

namespace Trial = Dimps::Game::Battle::Trial;
using Trial::TaskList;

TaskList::__publicMethods TaskList::publicMethods;

void Trial::Locate(HMODULE peRoot) {
	TaskList::Locate(peRoot);
}

void TaskList::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Construct = (PVOID)(peRootOffset + 0x258b80);
	*(PVOID*)&publicMethods.Destruct = (PVOID)(peRootOffset + 0x258b10);
	*(PVOID*)&publicMethods.Init = (PVOID)(peRootOffset + 0x258910);
	*(PVOID*)&publicMethods.Update = (PVOID)(peRootOffset + 0x258cc0);
	*(PVOID*)&publicMethods.Release = (PVOID)(peRootOffset + 0x2588b0);
	*(PVOID*)&publicMethods.Draw = (PVOID)(peRootOffset + 0x2588e0);
	*(PVOID*)&publicMethods.Advance = (PVOID)(peRootOffset + 0x258890);
	*(PVOID*)&publicMethods.SetTask = (PVOID)(peRootOffset + 0x259000);
	*(PVOID*)&publicMethods.SetTaskCursor = (PVOID)(peRootOffset + 0x2588f0);
	*(PVOID*)&publicMethods.ClearAllTask = (PVOID)(peRootOffset + 0x259170);
	*(PVOID*)&publicMethods.SetPosition = (PVOID)(peRootOffset + 0x259200);
}
