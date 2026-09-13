#include <windows.h>

#include "Dimps__Eva.hxx"
#include "Dimps__Game__Battle__Camera.hxx"

namespace Camera = Dimps::Game::Battle::Camera;
using Dimps::Eva::Task;
using Camera::Unit;

Unit::__privateMethods Unit::privateMethods;
Unit::__publicMethods Unit::publicMethods;
Unit::__staticMethods Unit::staticMethods;

void Camera::Locate(HMODULE peRoot) {
	Unit::Locate(peRoot);
}


Task** Unit::GetCamShakeTask(Unit* u) {
	return (Task**)((unsigned int)u + 0x110);
}

void Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.RecordToInternalMementoKey = (PVOID)(peRootOffset + 0x1719d0);
	*(PVOID*)&publicMethods.RestoreFromInternalMementoKey = (PVOID)(peRootOffset + 0x1717d0);
}