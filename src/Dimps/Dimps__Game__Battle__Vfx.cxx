#include <windows.h>
#include <string>

#include "Dimps__Game__Battle__Vfx.hxx"
#include "Dimps__Platform.hxx"

namespace Vfx = Dimps::Game::Battle::Vfx;
using Vfx::ColorFade;
using Vfx::ColorFadeData;
using Vfx::ColorFadeUnit;
using Vfx::Object;
using Vfx::ObjectContainer;
using Vfx::Particle;
using Vfx::ParticleContainer;
using Vfx::Trace;
using Vfx::TraceContainer;
using Vfx::Unit;

// Inlined in the MOV instruction at RVA 0x1bdd17, in the
// initialization for the container.
const unsigned int ObjectContainer::DEFAULT_LOOSE_OBJECT_COUNT = 0x20;

// First used inlined in the PUSH instruction at RVA 0x1bdd9f,
// in the initialization for the container. Note that unlike the
// other "default" counts in all three VFX container types, this
// isn't the consequence of default argument passing- this is
// actually a fixed number.
const unsigned int ObjectContainer::RESERVED_OBJECT_COUNT = 0x10;

// Inlined in the MOV instruction at RVA 0x1c0bd1, in the
// initialization for the container.
const unsigned int ParticleContainer::DEFAULT_PARTICLE_COUNT = 0x100;

// Inlined in the MOV instruction at RVA 0x1c2a51, in the
// initialization for the container.
const unsigned int TraceContainer::DEFAULT_TRACE_COUNT = 0x40;

ColorFade::__publicMethods ColorFade::publicMethods;
ColorFadeUnit::__publicMethods ColorFadeUnit::publicMethods;
ColorFadeUnit::__staticMethods ColorFadeUnit::staticMethods;
ObjectContainer::__publicMethods ObjectContainer::publicMethods;
ParticleContainer::__publicMethods ParticleContainer::publicMethods;
TraceContainer::__publicMethods TraceContainer::publicMethods;
Unit::__privateMethods Unit::privateMethods;
Unit::__publicMethods Unit::publicMethods;
Unit::__staticMethods Unit::staticMethods;

void Vfx::Locate(HMODULE peRoot) {
	ColorFade::Locate(peRoot);
	ColorFadeUnit::Locate(peRoot);
	Object::Locate(peRoot);
	ObjectContainer::Locate(peRoot);
	Particle::Locate(peRoot);
	ParticleContainer::Locate(peRoot);
	Trace::Locate(peRoot);
	TraceContainer::Locate(peRoot);
	Unit::Locate(peRoot);
}

void ColorFade::Locate(HMODULE peRoot){
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Spawn = (PVOID)(peRootOffset + 0x1ce7b0);
}

Dimps::Platform::list<ColorFadeData>* ColorFade::GetList(ColorFade* fade) {
	return (Dimps::Platform::list<ColorFadeData>*)((unsigned int)fade + 12);
}

void ColorFadeUnit::Locate(HMODULE peRoot){
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.GetFade = (PVOID)(peRootOffset + 0x1cd740);
	staticMethods.GetSingleton = (ColorFadeUnit* (*)())(peRootOffset + 0x1cdc40);
}

BYTE* Object::recordCopyCount = nullptr;
BYTE* Object::restoreCopyCount = nullptr;

void Object::Locate(HMODULE peRoot){
	unsigned int peRootOffset = (unsigned int)peRoot;

	// The `mov ecx, 0E4h` before each rep movsd in the memento record
	// (0x5BBC60) and restore (0x5BBCB0).
	recordCopyCount = (BYTE*)(peRootOffset + 0x1bbc7d);
	restoreCopyCount = (BYTE*)(peRootOffset + 0x1bbccd);
}

std::string* Object::GetNameTmp(Object* o) {
	return (std::string*)((unsigned int)o + 0x174);
}

std::string* Particle::GetNameTmp(Particle* p) {
	return (std::string*)((unsigned int)p + 0x110);
}

std::string* Trace::GetNameTmp(Trace* t) {
	return (std::string*)((unsigned int)t + 0x110);
}

DWORD ObjectContainer::GenerateFakeHandle(unsigned int index, bool isReservedObject) {
	if (isReservedObject) {
		return (index < RESERVED_OBJECT_COUNT) ? (index & 0xffff | EHT_OBJ_RESERVED) : INVALID_EFFECT_HANDLE;
	}
	else {
		return (index < DEFAULT_LOOSE_OBJECT_COUNT) ? index & 0xffff : INVALID_EFFECT_HANDLE;
	}
}

void ObjectContainer::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.GetObjectFromHandle = (PVOID)(peRootOffset + 0x1bdfd0);
}

void Particle::Locate(HMODULE peRoot){ }

DWORD ParticleContainer::GenerateFakeHandle(unsigned int index) {
	return (index < DEFAULT_PARTICLE_COUNT) ? (index & 0xffff | EHT_PARTICLE) : INVALID_EFFECT_HANDLE;
}

void ParticleContainer::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.GetParticleFromHandle = (PVOID)(peRootOffset + 0x1c0de0);
}

void Trace::Locate(HMODULE peRoot){ }

DWORD TraceContainer::GenerateFakeHandle(unsigned int index) {
	return (index < DEFAULT_TRACE_COUNT) ? (index & 0xffff | EHT_TRACE) : INVALID_EFFECT_HANDLE;
}

void TraceContainer::Locate(HMODULE peRoot){
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.GetTraceFromHandle = (PVOID)(peRootOffset + 0x1c2c10);
}

void Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.GetContainerByType = (PVOID)(peRootOffset + 0x1b6a70);
	*(PVOID*)&publicMethods.RecordToInternalMementoKey = (PVOID)(peRootOffset + 0x1b6800);
	*(PVOID*)&publicMethods.RestoreFromInternalMementoKey = (PVOID)(peRootOffset + 0x1b6840);
}