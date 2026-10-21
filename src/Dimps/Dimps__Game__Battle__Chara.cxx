#include <windows.h>

#include "Dimps__Game__Battle__Chara.hxx"

namespace Chara = Dimps::Game::Battle::Chara;
using Chara::Unit;
using Chara::Actor;
using Chara::Afterimage;

Actor::__publicMethods Actor::publicMethods;
Actor::__staticMethods Actor::staticMethods;

Unit::__publicMethods Unit::publicMethods;
Afterimage::__mementoableMethods Afterimage::mementoableMethods;

void Chara::Locate(HMODULE peRoot) {
	Actor::Locate(peRoot);
	Afterimage::Locate(peRoot);
	Unit::Locate(peRoot);
}

void Afterimage::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	*(PVOID*)&mementoableMethods.GetMementoSize = (PVOID)(peRootOffset + 0x162040);
	*(PVOID*)&mementoableMethods.RecordToMemento = (PVOID)(peRootOffset + 0x1620a0);
	*(PVOID*)&mementoableMethods.RestoreFromMemento = (PVOID)(peRootOffset + 0x162100);
}

// The IMementoable subobject is at object +0x60 and the key at +0x6B10.
Chara::GameMementoKey* Afterimage::GetKey(Afterimage* mementoable) {
	return (GameMementoKey*)((unsigned int)mementoable - 0x60 + 0x6b10);
}

void Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	*(PVOID*)&publicMethods.GetActorByIndex = (PVOID)(peRootOffset + 0x165d10);
	*(PVOID*)&publicMethods.CanStoreMemento_MaybeActorExists = (PVOID)(peRootOffset + 0x165cd0);
	*(PVOID*)&publicMethods.RecordToInternalMementoKey = (PVOID)(peRootOffset + 0x1633a0);
	*(PVOID*)&publicMethods.RestoreFromInternalMementoKey = (PVOID)(peRootOffset + 0x1633f0);
}

unsigned char* Actor::GetBodyBox(Actor* a, BodyList list, unsigned index) {
	return (unsigned char*)a + (list == BodyList::Strike ? 0x26e0 : 0x44e0) + index * 0xa0;
}

// The masks in effect this frame, after the push boxes' at +0x6810.
unsigned int Actor::GetBodyMask(Actor* a, BodyList list) {
	return *(unsigned int*)((unsigned char*)a + (list == BodyList::Strike ? 0x6814 : 0x6818));
}

// Six list pointers from +0x130; a list keeps its first box at +0x20 and its
// count at +0x2C.
unsigned char* Actor::GetAttackBoxes(const void* object, unsigned list, unsigned& count) {
	unsigned char* head = *(unsigned char* const*)((const unsigned char*)object + 0x130 + list * 4);
	count = head ? *(unsigned int*)(head + 0x2c) : 0;
	return count ? *(unsigned char**)(head + 0x20) : nullptr;
}

void Actor::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	*(PVOID*)(&publicMethods.GetActorID) = (PVOID)(peRootOffset + 0x16c6a0);
	*(PVOID*)(&publicMethods.GetComboDamage) = (PVOID)(peRootOffset + 0x142410);
	*(PVOID*)(&publicMethods.GetCurrentSide) = (PVOID)(peRootOffset + 0x141d20);
	*(PVOID*)(&publicMethods.GetDamage) = (PVOID)(peRootOffset + 0x1423d0);
	*(PVOID*)(&publicMethods.GetStatus) = (PVOID)(peRootOffset + 0x142520);
	*(PVOID*)(&publicMethods.GetActionID) = (PVOID)(peRootOffset + 0x12d700);
	*(PVOID*)(&publicMethods.GetActionFrame) = (PVOID)(peRootOffset + 0x12d720);
	*(PVOID*)(&publicMethods.GetActionPosture) = (PVOID)(peRootOffset + 0x141ea0);
	*(PVOID*)(&publicMethods.GetBasicActionInhibit) = (PVOID)(peRootOffset + 0x142390);
	*(PVOID*)(&publicMethods.GetActionScript) = (PVOID)(peRootOffset + 0x12da50);
	*(PVOID*)(&publicMethods.GetCommandMove) = (PVOID)(peRootOffset + 0x1423b0);
	*(PVOID*)(&publicMethods.GetFramesUntilActionable) = (PVOID)(peRootOffset + 0x142480);

	*(PVOID*)(&publicMethods.GetVitalityAmt_FixedPoint) = (PVOID)(peRootOffset + 0x141ee0);
	*(PVOID*)(&publicMethods.GetVitalityMax_FixedPoint) = (PVOID)(peRootOffset + 0x141f00);
	*(PVOID*)(&publicMethods.GetVitalityPct_FixedPoint) = (PVOID)(peRootOffset + 0x141f20);

	*(PVOID*)(&publicMethods.GetRecoverableVitalityAmt_FixedPoint) = (PVOID)(peRootOffset + 0x141f80);
	*(PVOID*)(&publicMethods.GetRecoverableVitalityMax_FixedPoint) = (PVOID)(peRootOffset + 0x141fa0);
	*(PVOID*)(&publicMethods.GetRecoverableVitalityPct_FixedPoint) = (PVOID)(peRootOffset + 0x141fc0);

	*(PVOID*)(&publicMethods.GetSuperComboAmt_FixedPoint) = (PVOID)(peRootOffset + 0x142020);
	*(PVOID*)(&publicMethods.GetSuperComboMax_FixedPoint) = (PVOID)(peRootOffset + 0x142040);
	*(PVOID*)(&publicMethods.GetSuperComboPct_FixedPoint) = (PVOID)(peRootOffset + 0x142060);

	*(PVOID*)(&publicMethods.GetSCTimeAmt_FixedPoint) = (PVOID)(peRootOffset + 0x1420c0);
	*(PVOID*)(&publicMethods.GetSCTimeMax_FixedPoint) = (PVOID)(peRootOffset + 0x1420e0);
	*(PVOID*)(&publicMethods.GetSCTimePct_FixedPoint) = (PVOID)(peRootOffset + 0x142100);

	*(PVOID*)(&publicMethods.GetRevengeAmt_FixedPoint) = (PVOID)(peRootOffset + 0x142180);
	*(PVOID*)(&publicMethods.GetRevengeMax_FixedPoint) = (PVOID)(peRootOffset + 0x1421a0);
	*(PVOID*)(&publicMethods.GetRevengePct_FixedPoint) = (PVOID)(peRootOffset + 0x1421c0);

	*(PVOID*)(&publicMethods.GetUCTimeAmt_FixedPoint) = (PVOID)(peRootOffset + 0x142220);
	*(PVOID*)(&publicMethods.GetUCTimeMax_FixedPoint) = (PVOID)(peRootOffset + 0x142240);
	*(PVOID*)(&publicMethods.GetUCTimePct_FixedPoint) = (PVOID)(peRootOffset + 0x142260);

	*(PVOID*)(&publicMethods.GetCurrentRootPosition) = (PVOID)(peRootOffset + 0x12d660);
	*(PVOID*)(&publicMethods.GetCurrentBonePositionByID) = (PVOID)(peRootOffset + 0x1427a0);

	staticMethods.GetBoneLabelByID = (char* (*)(int))(peRootOffset + 0x163260);
	staticMethods.GetLocatorPairEnd = (int (*)(unsigned int))(peRootOffset + 0x1632a0);
	staticMethods.ResetAfterMemento = (void(*)(Actor*))(peRootOffset + 0x151800);
}
