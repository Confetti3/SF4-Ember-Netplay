#pragma once

// Layout of a Chara::Afterimage memento once Ember appends the Action::Actor
// state the engine's Afterimage override leaves out (see
// sf4e__Game__Battle__Chara.cxx for the root cause). One engine slot holds:
//
//   [Afterimage::Memento, afterimageSize][pad to 16][Actor payload, actorSize]...[Trailer, last 32 bytes]
//
// The trailer sits at the end of the slot, a position the key's allocation
// fixes, so restore finds it without any engine formula. This file is pure:
// the hook passes the key's raw fields and the memento pointer, gets a typed
// decision with offsets, and calls the engine itself. AfterimageMementoTest
// covers every accept and reject on byte buffers laid out at the real offsets.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sf4e { namespace afterimage {

// The fixed part of the engine's Afterimage memento (0x6910 in 0x562040):
// its state, texture header and 32 dwords, before the captured textures.
constexpr uint64_t kNativeFixedBytes = 26896;
constexpr uint64_t kTrailerBytes = 32;
constexpr uint32_t kTrailerMagic = 0x4D494145; // "EAIM"
// Action::Actor's memento (size 0x52B730): 576 bytes plus 272 per collision
// box, with the five list counts at +4..+20.
constexpr uint64_t kActorFixedBytes = 576;
constexpr uint64_t kActorBoxBytes = 272;
constexpr int kActorLists = 5;
constexpr size_t kActorCountsOffset = 4;
// GameMementoKey::Initialize (0x52FD40) allocates numMementos * (size + 12):
// each slot's memento, then a 12-byte metadata entry per slot.
constexpr uint64_t kMetadataBytes = 12;
// The engine only ever uses one slot per key; this bounds a corrupt count.
constexpr int64_t kMaxMementos = 64;

// Afterimage's own restore (0x562100) copies its state, 0x6870 bytes from
// memento +16 into object +676, then restores its pose ring from memento
// +26752: a 144-byte header, then the captured poses. The pose ring holds
// snapshots of the owner's skeleton taken while drawing (0x562820), read
// only by the draw (0x562A80) and cleared by activation and Update, so Ember
// restores the state and leaves the ring as the screen last drew it.
constexpr uint64_t kMementoableOffset = 0x60;
constexpr uint64_t kStateMementoOffset = 16;
constexpr uint64_t kStateObjectOffset = 676;
constexpr uint64_t kStateBytes = 0x6870;
constexpr uint64_t kPoseRingMementoOffset = 26752;
constexpr uint64_t kPoseRingHeaderBytes = 144;
static_assert(kStateMementoOffset + kStateBytes == kPoseRingMementoOffset, "the state ends where the pose ring starts");
static_assert(kPoseRingMementoOffset + kPoseRingHeaderBytes == kNativeFixedBytes, "the fixed part ends with the ring header");

enum class Kind : uint32_t { NativeOnly = 1, NativeAndActor = 2 };

struct Trailer {
	uint32_t magic;
	uint32_t kind;
	uint32_t afterimageSize;
	uint32_t actorOffset;
	uint32_t actorSize;
	uint32_t reserved[3];
};
static_assert(sizeof(Trailer) == kTrailerBytes, "the trailer is exactly 32 bytes");

constexpr uint64_t AlignUp16(uint64_t size) { return (size + 15) & ~uint64_t(15); }

// One engine slot, validated before any of it is read. `listed` holds the
// memento pointer of each of the key's metadata entries, in slot order.
struct SlotView {
	uint8_t* m = nullptr;
	uint64_t size = 0;
	const char* reject = "slot not built"; // null only once Make verified it

	bool Valid() const { return reject == nullptr; }

	static SlotView Make(uint8_t* m, const uint8_t* mementos, int64_t sizeAllocated, int64_t numMementos,
		const uint8_t* const* listed, size_t listedCount) {
		SlotView slot;
		if (!m || !mementos) return Reject(slot, "null memento or buffer");
		if (numMementos <= 0 || numMementos > kMaxMementos) return Reject(slot, "memento count out of range");
		if (!listed || listedCount != uint64_t(numMementos)) return Reject(slot, "metadata does not match the memento count");
		if (sizeAllocated <= 0 || sizeAllocated % numMementos != 0) return Reject(slot, "allocation is not whole slots");
		const uint64_t perSlot = uint64_t(sizeAllocated / numMementos);
		if (perSlot < kMetadataBytes + kNativeFixedBytes + kTrailerBytes) return Reject(slot, "slot smaller than the fixed memento");
		const uint64_t size = perSlot - kMetadataBytes;
		for (size_t i = 0; i < listedCount; i++) {
			if (listed[i] != m) continue;
			if (m != mementos + i * size) return Reject(slot, "memento is not at its slot");
			slot.m = m;
			slot.size = size;
			slot.reject = nullptr;
			return slot;
		}
		return Reject(slot, "memento is not in its key");
	}

private:
	static SlotView Reject(SlotView slot, const char* reason) {
		slot.reject = reason;
		return slot;
	}
};

enum class RecordDecision { NativeAndActor, NativeOnly, NoSlot };

struct RecordPlan {
	RecordDecision decision;
	uint64_t actorOffset;
	const char* reason; // why the Actor part is left out
};

// Called after the engine recorded its own part. `afterimageSize` is the
// engine's own size for it, `actorSize` Action::Actor's.
inline RecordPlan PlanRecord(const SlotView& slot, uint64_t afterimageSize, uint64_t actorSize) {
	if (!slot.Valid()) return { RecordDecision::NoSlot, 0, slot.reject };
	// A valid slot holds at least the fixed memento and a trailer, so these
	// subtractions cannot wrap.
	const uint64_t room = slot.size - kTrailerBytes;
	if (afterimageSize > room) {
		return { RecordDecision::NoSlot, 0, "no room for a trailer after the engine memento" };
	}
	if (afterimageSize < kNativeFixedBytes) {
		return { RecordDecision::NativeOnly, 0, "engine memento smaller than its fixed part" };
	}
	const uint64_t actorOffset = AlignUp16(afterimageSize);
	if (actorSize < kActorFixedBytes || actorOffset > room || actorSize != room - actorOffset) {
		return { RecordDecision::NativeOnly, 0, "slot was not sized for this Actor memento" };
	}
	return { RecordDecision::NativeAndActor, actorOffset, nullptr };
}

inline void WriteTrailer(const SlotView& slot, Kind kind, uint64_t afterimageSize, uint64_t actorOffset, uint64_t actorSize) {
	Trailer trailer = {};
	trailer.magic = kTrailerMagic;
	trailer.kind = uint32_t(kind);
	trailer.afterimageSize = uint32_t(afterimageSize);
	trailer.actorOffset = uint32_t(actorOffset);
	trailer.actorSize = uint32_t(actorSize);
	std::memcpy(slot.m + slot.size - kTrailerBytes, &trailer, sizeof(trailer));
}

struct RestorePlan {
	bool restoreActor;
	uint64_t actorOffset;
	const char* reason; // why the Actor part is skipped
	bool report;        // false when record already reported it
};

// Called before either restore. Each read happens only after the checks
// that bound it.
inline RestorePlan PlanRestore(const SlotView& slot) {
	if (!slot.Valid()) return { false, 0, slot.reject, true };
	Trailer trailer;
	std::memcpy(&trailer, slot.m + slot.size - kTrailerBytes, sizeof(trailer));
	if (trailer.magic != kTrailerMagic) return { false, 0, "no trailer", true };
	if (trailer.kind == uint32_t(Kind::NativeOnly)) return { false, 0, "recorded without the Actor part", false };
	if (trailer.kind != uint32_t(Kind::NativeAndActor)) return { false, 0, "unknown trailer kind", true };
	const uint64_t afterimageSize = trailer.afterimageSize;
	const uint64_t actorOffset = trailer.actorOffset;
	const uint64_t actorSize = trailer.actorSize;
	if (afterimageSize < kNativeFixedBytes) return { false, 0, "trailer engine size below its fixed part", true };
	if (actorOffset != AlignUp16(afterimageSize)) return { false, 0, "trailer Actor offset misplaced", true };
	if (actorOffset + actorSize + kTrailerBytes != slot.size) return { false, 0, "trailer does not describe this slot", true };
	if (actorSize < kActorFixedBytes) return { false, 0, "Actor payload smaller than its fixed part", true };
	uint64_t boxes = 0;
	for (int i = 0; i < kActorLists; i++) {
		int32_t count;
		std::memcpy(&count, slot.m + actorOffset + kActorCountsOffset + 4 * i, sizeof(count));
		if (count < 0) return { false, 0, "negative Actor box count", true };
		boxes += uint64_t(count);
	}
	if (kActorFixedBytes + kActorBoxBytes * boxes != actorSize) return { false, 0, "Actor payload does not match its box counts", true };
	return { true, actorOffset, nullptr, true };
}

} }
