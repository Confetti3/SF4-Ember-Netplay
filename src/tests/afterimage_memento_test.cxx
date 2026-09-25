// The combined Afterimage memento layout on byte buffers laid out the way
// GameMementoKey::Initialize (0x52FD40) allocates them: numMementos slots of
// `size` bytes, then 12 bytes of metadata per slot.
#include <cstdint>
#include <cstring>
#include <vector>

#include "../common/AfterimageMemento.hxx"
#include "test_support.hxx"

namespace am = sf4e::afterimage;

struct Key {
	std::vector<uint8_t> buffer;
	uint64_t size;
	int64_t count;

	Key(uint64_t slotSize, int64_t slots) : buffer(size_t(slots * (slotSize + am::kMetadataBytes)), 0), size(slotSize), count(slots) {}
	uint8_t* Slot(int64_t i) { return buffer.data() + i * size; }
	int64_t Allocated() const { return count * int64_t(size + am::kMetadataBytes); }
	std::vector<const uint8_t*> Listed() {
		std::vector<const uint8_t*> listed;
		for (int64_t i = 0; i < count; i++) listed.push_back(Slot(i));
		return listed;
	}
	am::SlotView View(int64_t i) {
		const auto listed = Listed();
		return am::SlotView::Make(Slot(i), buffer.data(), Allocated(), count, listed.data(), listed.size());
	}
};

// Sizes that match: an engine memento with two textures of odd size, and an
// Actor memento with three collision boxes.
constexpr uint64_t kAfterimage = am::kNativeFixedBytes + 2 * 1001;
constexpr uint64_t kActor = am::kActorFixedBytes + 3 * am::kActorBoxBytes;
constexpr uint64_t kSlot = am::AlignUp16(kAfterimage) + kActor + am::kTrailerBytes;

static void TestSlotContract() {
	// A slot nobody built is invalid, and plans never read through it.
	const am::SlotView unbuilt;
	CHECK(!unbuilt.Valid() && unbuilt.reject != nullptr);
	CHECK(am::PlanRecord(unbuilt, kAfterimage, kActor).decision == am::RecordDecision::NoSlot);
	CHECK(!am::PlanRestore(unbuilt).restoreActor && am::PlanRestore(unbuilt).report);

	Key key(kSlot, 1);
	CHECK(key.View(0).Valid());
	Key two(kSlot, 2);
	CHECK(two.View(1).Valid() && two.View(1).m == two.Slot(1) && two.View(1).size == kSlot);

	const auto reject = [](const am::SlotView& slot) { return !slot.Valid() && slot.reject != nullptr; };
	CHECK(reject(am::SlotView::Make(nullptr, key.buffer.data(), key.Allocated(), 1, key.Listed().data(), 1)));
	CHECK(reject(am::SlotView::Make(key.Slot(0), nullptr, key.Allocated(), 1, key.Listed().data(), 1)));
	CHECK(reject(am::SlotView::Make(key.Slot(0), key.buffer.data(), key.Allocated(), 0, nullptr, 0)));
	CHECK(reject(am::SlotView::Make(key.Slot(0), key.buffer.data(), key.Allocated(), -1, key.Listed().data(), 1)));
	CHECK(reject(am::SlotView::Make(key.Slot(0), key.buffer.data(), key.Allocated(), am::kMaxMementos + 1, key.Listed().data(), 1)));
	CHECK(reject(am::SlotView::Make(key.Slot(0), key.buffer.data(), key.Allocated(), 2, key.Listed().data(), 1)));   // metadata count
	CHECK(reject(am::SlotView::Make(key.Slot(0), key.buffer.data(), 0, 1, key.Listed().data(), 1)));
	CHECK(reject(am::SlotView::Make(two.Slot(0), two.buffer.data(), two.Allocated() + 1, 2, two.Listed().data(), 2))); // not whole slots
	Key tiny(am::kNativeFixedBytes + am::kTrailerBytes - 1, 1);
	CHECK(reject(tiny.View(0)));
	// A pointer the key does not list, and a listed pointer off its slot.
	CHECK(reject(am::SlotView::Make(key.Slot(0) + 16, key.buffer.data(), key.Allocated(), 1, key.Listed().data(), 1)));
	std::vector<const uint8_t*> shifted = { key.Slot(0) + 16 };
	CHECK(reject(am::SlotView::Make(key.Slot(0) + 16, key.buffer.data(), key.Allocated(), 1, shifted.data(), 1)));
}

static void TestRecordPlan() {
	Key key(kSlot, 1);
	const am::SlotView slot = key.View(0);
	am::RecordPlan plan = am::PlanRecord(slot, kAfterimage, kActor);
	CHECK(plan.decision == am::RecordDecision::NativeAndActor && plan.actorOffset == am::AlignUp16(kAfterimage) && !plan.reason);
	CHECK(plan.actorOffset % 16 == 0);

	// The slot equation must hold exactly.
	CHECK(am::PlanRecord(slot, kAfterimage, kActor + am::kActorBoxBytes).decision == am::RecordDecision::NativeOnly);
	CHECK(am::PlanRecord(slot, kAfterimage, kActor - am::kActorBoxBytes).decision == am::RecordDecision::NativeOnly);
	CHECK(am::PlanRecord(slot, kAfterimage + 16, kActor).decision == am::RecordDecision::NativeOnly);
	CHECK(am::PlanRecord(slot, kAfterimage, am::kActorFixedBytes - 1).decision == am::RecordDecision::NativeOnly);
	// An engine memento below its fixed part keeps a trailer but no Actor part.
	CHECK(am::PlanRecord(slot, am::kNativeFixedBytes - 1, kActor).decision == am::RecordDecision::NativeOnly);
	// No room for a trailer after the engine's own bytes.
	CHECK(am::PlanRecord(slot, kSlot - am::kTrailerBytes + 1, kActor).decision == am::RecordDecision::NoSlot);
	// 64-bit overflow inputs never wrap into acceptance.
	CHECK(am::PlanRecord(slot, UINT64_MAX, kActor).decision == am::RecordDecision::NoSlot);
	CHECK(am::PlanRecord(slot, kAfterimage, UINT64_MAX).decision == am::RecordDecision::NativeOnly);
	CHECK(am::PlanRecord(slot, UINT64_MAX - 15, 0).decision == am::RecordDecision::NoSlot);
	// An invalid slot writes nothing.
	CHECK(am::PlanRecord(Key(kSlot, 1).View(0), kAfterimage, kActor).decision == am::RecordDecision::NativeAndActor);
	am::SlotView invalid;
	invalid.reject = "test";
	CHECK(am::PlanRecord(invalid, kAfterimage, kActor).decision == am::RecordDecision::NoSlot);
}

static void WriteCounts(const am::SlotView& slot, uint64_t actorOffset, const int32_t (&counts)[am::kActorLists]) {
	std::memcpy(slot.m + actorOffset + am::kActorCountsOffset, counts, sizeof(counts));
}

static void TestRoundTrips() {
	// Accepted: the Actor part comes back at the offset it was written to.
	{
		Key key(kSlot, 1);
		const am::SlotView slot = key.View(0);
		const am::RecordPlan plan = am::PlanRecord(slot, kAfterimage, kActor);
		CHECK(plan.decision == am::RecordDecision::NativeAndActor);
		WriteCounts(slot, plan.actorOffset, { 1, 0, 2, 0, 0 });
		am::WriteTrailer(slot, am::Kind::NativeAndActor, kAfterimage, plan.actorOffset, kActor);
		const am::RestorePlan restore = am::PlanRestore(slot);
		CHECK(restore.restoreActor && restore.actorOffset == plan.actorOffset && !restore.reason);
	}
	// Record rejected: the trailer says so, and restore does not report twice.
	{
		Key key(kSlot, 1);
		const am::SlotView slot = key.View(0);
		const am::RecordPlan plan = am::PlanRecord(slot, kAfterimage, kActor + am::kActorBoxBytes);
		CHECK(plan.decision == am::RecordDecision::NativeOnly && plan.reason);
		am::WriteTrailer(slot, am::Kind::NativeOnly, kAfterimage, 0, 0);
		const am::RestorePlan restore = am::PlanRestore(slot);
		CHECK(!restore.restoreActor && !restore.report);
	}
	// Boundary box counts: empty lists and large ones.
	for (int32_t perList : { 0, 64 }) {
		const uint64_t actor = am::kActorFixedBytes + am::kActorBoxBytes * uint64_t(perList) * am::kActorLists;
		const uint64_t slotSize = am::AlignUp16(kAfterimage) + actor + am::kTrailerBytes;
		Key key(slotSize, 1);
		const am::SlotView slot = key.View(0);
		const am::RecordPlan plan = am::PlanRecord(slot, kAfterimage, actor);
		CHECK(plan.decision == am::RecordDecision::NativeAndActor);
		WriteCounts(slot, plan.actorOffset, { perList, perList, perList, perList, perList });
		am::WriteTrailer(slot, am::Kind::NativeAndActor, kAfterimage, plan.actorOffset, actor);
		CHECK(am::PlanRestore(slot).restoreActor);
	}
}

// Each malformed trailer or payload is rejected before the Actor restore.
static void TestRestoreRejects() {
	const uint64_t offset = am::AlignUp16(kAfterimage);
	const auto prepared = [&](Key& key) {
		const am::SlotView slot = key.View(0);
		WriteCounts(slot, offset, { 3, 0, 0, 0, 0 });
		am::WriteTrailer(slot, am::Kind::NativeAndActor, kAfterimage, offset, kActor);
		CHECK(am::PlanRestore(slot).restoreActor);
		return slot;
	};
	const auto trailer = [](const am::SlotView& slot) {
		return reinterpret_cast<am::Trailer*>(slot.m + slot.size - am::kTrailerBytes);
	};
	const auto rejected = [](const am::SlotView& slot) {
		const am::RestorePlan plan = am::PlanRestore(slot);
		return !plan.restoreActor && plan.reason && plan.report;
	};
	{ Key key(kSlot, 1); const auto slot = key.View(0); CHECK(rejected(slot)); }                        // no trailer
	{ Key key(kSlot, 1); auto slot = prepared(key); trailer(slot)->magic ^= 1; CHECK(rejected(slot)); }
	{ Key key(kSlot, 1); auto slot = prepared(key); trailer(slot)->kind = 7; CHECK(rejected(slot)); }
	{ Key key(kSlot, 1); auto slot = prepared(key); trailer(slot)->afterimageSize = am::kNativeFixedBytes - 1; CHECK(rejected(slot)); }
	{ Key key(kSlot, 1); auto slot = prepared(key); trailer(slot)->actorOffset += 16; CHECK(rejected(slot)); }
	{ Key key(kSlot, 1); auto slot = prepared(key); trailer(slot)->actorSize += 16; CHECK(rejected(slot)); }
	{ Key key(kSlot, 1); auto slot = prepared(key); trailer(slot)->actorSize = 0xFFFFFFFF; CHECK(rejected(slot)); }
	{ Key key(kSlot, 1); auto slot = prepared(key); WriteCounts(slot, offset, { 2, 0, 0, 0, 0 }); CHECK(rejected(slot)); }
	{ Key key(kSlot, 1); auto slot = prepared(key); WriteCounts(slot, offset, { 4, -1, 0, 0, 0 }); CHECK(rejected(slot)); }
	// An Actor memento below its fixed part, in a slot sized for it.
	{
		const uint64_t small = am::kActorFixedBytes - 16;
		Key key(offset + small + am::kTrailerBytes, 1);
		const am::SlotView slot = key.View(0);
		am::WriteTrailer(slot, am::Kind::NativeAndActor, kAfterimage, offset, small);
		CHECK(rejected(slot));
	}
	// An invalid slot is rejected and reported.
	{ am::SlotView invalid; invalid.reject = "test"; CHECK(rejected(invalid)); }
}

int main() {
	TestSlotContract();
	TestRecordPlan();
	TestRoundTrips();
	TestRestoreRejects();
	std::printf("Afterimage memento layout passed\n");
}
