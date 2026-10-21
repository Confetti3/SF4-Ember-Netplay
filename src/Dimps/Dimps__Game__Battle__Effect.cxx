#include <windows.h>

#include "Dimps__Game__Battle__Effect.hxx"

namespace Effect = Dimps::Game::Battle::Effect;
using Effect::ActorManager;
using Effect::Unit;

ActorManager::__publicMethods ActorManager::publicMethods;
Unit::__privateMethods Unit::privateMethods;
Unit::__publicMethods Unit::publicMethods;
Unit::__staticMethods Unit::staticMethods;

void Effect::Locate(HMODULE peRoot) {
	ActorManager::Locate(peRoot);
	Unit::Locate(peRoot);
}

void ActorManager::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)(&publicMethods.GetMementoSize) = (PVOID)(peRootOffset + 0x16ca60);
}

void* Unit::GetFirstEffect(Unit* u, unsigned side) {
	unsigned char* manager = *(unsigned char**)((unsigned char*)u + 8 + side * 4);
	return manager && *(unsigned int*)(manager + 0x8c) ? *(void**)(manager + 0x84) : nullptr;
}

void Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)(&publicMethods.RecordToInternalMementoKey) = (PVOID)(peRootOffset + 0x16ee70);
	*(PVOID*)(&publicMethods.RestoreFromInternalMementoKey) = (PVOID)(peRootOffset + 0x16eea0);
}