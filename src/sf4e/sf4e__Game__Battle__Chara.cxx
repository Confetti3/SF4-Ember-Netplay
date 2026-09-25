// Rollback for the fighters' afterimages (the shadows of Yang's Seiei Enbu,
// Yun's Genei Jin, Rose's Soul Illusion and every other shadow move).
//
// Dimps::Game::Battle::Chara::Afterimage derives from Action::Actor (base
// ctor 0x52D570). Action::Actor's memento saves the actor's state: its
// scalars (Actor +16..+563, which include the animation frame counters at
// object +0x148..+0x150), its five collision-box lists, and, by the same
// memento id, its action engine at *(Actor+80) (size 0x52B730, record
// 0x52B7C0, restore 0x52C0C0). Afterimage overrides that whole interface
// (vtable 0x94fafc: size 0x562040, record 0x5620A0, restore 0x562100) with a
// copy of its own fields (object +676..) and captured textures, and never
// calls the base. Rollback exposes it: every load leaves each shadow's
// animation frame, hitboxes and engine on the discarded timeline:
// the shadows freeze or trail, and their hits land on different frames on
// each PC (the Seiei Enbu desync).
//
// These detours append Action::Actor's memento after Afterimage's own, in the
// same engine slot, with a trailer that describes the layout
// (common/AfterimageMemento.hxx). The size is recomputed before each record:
// every record-by-id (Afterimage 0x562950, and the engine the base record
// chains) calls GameMementoKey::Initialize 0x52FD40, which clears the key,
// asks the size function, then allocates, immediately before the record.
//
// Afterimage's own record always runs, exactly as before; only the added
// Actor part depends on validation. When it cannot be written or found the
// Actor state stays as the engine leaves it today, and Game::MementoFailure
// makes Ember's own save or load report incomplete.
//
// Restore copies Afterimage's own state as the engine does but leaves out its
// pose ring. The ring holds the poses the draw captured from the owner, one
// per drawn frame. Frames a rollback re-simulates are never drawn, so
// restoring the ring discarded the poses of every drawn frame after the load
// point: the shadows fell behind the fighter and, under the stress harness's
// load every 4 frames, stood still. Nothing in the simulation reads the ring.
#include <windows.h>
#include <detours/detours.h>

#include "spdlog/spdlog.h"

#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle__Action.hxx"
#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../common/AfterimageMemento.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__Game__Battle__Chara.hxx"

namespace am = sf4e::afterimage;
namespace fChara = sf4e::Game::Battle::Chara;
namespace rChara = Dimps::Game::Battle::Chara;
using fAfterimage = fChara::Afterimage;
using rAfterimage = rChara::Afterimage;
using rActor = Dimps::Game::Battle::Action::Actor;
using rKey = Dimps::Game::GameMementoKey;

namespace {

rAfterimage* Native(fAfterimage* self) { return reinterpret_cast<rAfterimage*>(self); }
rActor* Base(fAfterimage* self) { return reinterpret_cast<rActor*>(self); }

am::SlotView Slot(fAfterimage* self, void* memento) {
	const rKey* key = rAfterimage::GetKey(Native(self));
	const uint8_t* listed[am::kMaxMementos];
	size_t listedCount = 0;
	if (key->metadata && key->numMementos > 0 && key->numMementos <= am::kMaxMementos) {
		for (int i = 0; i < key->numMementos; i++) listed[listedCount++] = (const uint8_t*)key->metadata[i].memento;
	}
	return am::SlotView::Make((uint8_t*)memento, (const uint8_t*)key->mementos, key->sizeAllocated,
		key->numMementos, listedCount ? listed : nullptr, listedCount);
}

// Afterimage's own restore (0x562100) without its last step, the pose ring
// restore (0x561D50).
int RestoreState(fAfterimage* self, void* memento) {
	if (!memento) return 0;
	uint8_t* object = reinterpret_cast<uint8_t*>(self) - am::kMementoableOffset;
	std::memcpy(object + am::kStateObjectOffset, static_cast<const uint8_t*>(memento) + am::kStateMementoOffset, am::kStateBytes);
	return 1;
}

// Each distinct reason is logged once per process; the reasons are literals.
void ReportOnce(const char* operation, const char* reason) {
	static const char* logged[16];
	static size_t count = 0;
	for (size_t i = 0; i < count; i++) {
		if (logged[i] == reason) return;
	}
	if (count < sizeof(logged) / sizeof(logged[0])) logged[count++] = reason;
	spdlog::error("Rollback: afterimage {} without its Actor state: {}", operation, reason);
}

}

void fChara::Install() {
	Afterimage::Install();
}

void fAfterimage::Install() {
	size_t (fAfterimage::* _fGetMementoSize)() = &GetMementoSize;
	int (fAfterimage::* _fRecordToMemento)(void*, rKey::MementoID*) = &RecordToMemento;
	int (fAfterimage::* _fRestoreFromMemento)(void*, rKey::MementoID*) = &RestoreFromMemento;
	DetourAttach((PVOID*)&rAfterimage::mementoableMethods.GetMementoSize, *(PVOID*)&_fGetMementoSize);
	DetourAttach((PVOID*)&rAfterimage::mementoableMethods.RecordToMemento, *(PVOID*)&_fRecordToMemento);
	DetourAttach((PVOID*)&rAfterimage::mementoableMethods.RestoreFromMemento, *(PVOID*)&_fRestoreFromMemento);
}

size_t fAfterimage::GetMementoSize() {
	const uint64_t afterimageSize = (Native(this)->*rAfterimage::mementoableMethods.GetMementoSize)();
	const uint64_t actorSize = (Base(this)->*rActor::publicMethods.GetMementoSize)();
	return size_t(am::AlignUp16(afterimageSize) + actorSize + am::kTrailerBytes);
}

int fAfterimage::RecordToMemento(void* memento, rKey::MementoID* id) {
	const int result = (Native(this)->*rAfterimage::mementoableMethods.RecordToMemento)(memento, id);
	const uint64_t afterimageSize = (Native(this)->*rAfterimage::mementoableMethods.GetMementoSize)();
	const uint64_t actorSize = (Base(this)->*rActor::publicMethods.GetMementoSize)();
	const am::SlotView slot = Slot(this, memento);
	const am::RecordPlan plan = am::PlanRecord(slot, afterimageSize, actorSize);
	if (plan.decision == am::RecordDecision::NativeAndActor &&
		(Base(this)->*rActor::publicMethods.RecordToMemento)((uint8_t*)memento + plan.actorOffset, id)) {
		am::WriteTrailer(slot, am::Kind::NativeAndActor, afterimageSize, plan.actorOffset, actorSize);
		return result;
	}
	sf4e::Game::MementoFailure::record = true;
	ReportOnce("record", plan.reason ? plan.reason : "the Actor record returned 0");
	if (plan.decision != am::RecordDecision::NoSlot) {
		am::WriteTrailer(slot, am::Kind::NativeOnly, afterimageSize, 0, 0);
	}
	return result;
}

int fAfterimage::RestoreFromMemento(void* memento, rKey::MementoID* id) {
	const am::RestorePlan plan = am::PlanRestore(Slot(this, memento));
	if (plan.restoreActor) {
		if (!(Base(this)->*rActor::publicMethods.RestoreFromMemento)((uint8_t*)memento + plan.actorOffset, id)) {
			sf4e::Game::MementoFailure::restore = true;
			ReportOnce("restore", "the Actor restore returned 0");
		}
	} else if (plan.report) {
		sf4e::Game::MementoFailure::restore = true;
		ReportOnce("restore", plan.reason);
	}
	return RestoreState(this, memento);
}
