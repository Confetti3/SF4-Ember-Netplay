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

void Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)(&publicMethods.RecordToInternalMementoKey) = (PVOID)(peRootOffset + 0x16ee70);
	*(PVOID*)(&publicMethods.RestoreFromInternalMementoKey) = (PVOID)(peRootOffset + 0x16eea0);
}