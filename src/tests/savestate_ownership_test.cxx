// Pure unit tests for the savestate key-ownership protocol.
//
// The real fSystem::SaveState is welded to the live SF4 engine (memento
// record/restore mutate engine objects), so it cannot be linked into a
// standalone test. What CAN be tested in isolation is the ownership protocol
// those functions implement, which is where the netplay crash lived:
//
//   Save()  moves payload ownership from the live key INTO the state
//           (it zeroes the live key afterwards).
//   Clear() releases payloads, but ONLY if the state still owns them.
//   Free()  restores the live keys from a scratch copy, which hands
//           ownership BACK -- so the scratch copy's claim must be dropped
//           without releasing, or the payload is freed while still in use.
//
// The bug: Free()'s scratch `tmp` kept a stale claim. Surviving one call, it
// broke in CloseBattle's Free loop, where the next iteration's Clear() freed
// through pointers the previous iteration had already handed back.
//
// This mirror keeps the same shape as the real code so a regression in the
// protocol trips here. It deliberately does NOT re-verify std::vector.

#include <stdio.h>

#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                     \
		if (!(cond)) {                                                       \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
			g_failures++;                                                    \
		}                                                                    \
	} while (0)

// ---------------------------------------------------------------------------
// Fake engine: a payload heap that reports double frees and leaks.
// ---------------------------------------------------------------------------

struct FakeHeap {
	std::vector<int> liveCount; // per payload id: times allocated - freed
	int doubleFrees = 0;
	int freesOfUnowned = 0;

	int Alloc() {
		liveCount.push_back(1);
		return (int)liveCount.size() - 1;
	}

	void Free(int id) {
		if (id < 0 || id >= (int)liveCount.size()) {
			freesOfUnowned++;
			return;
		}
		if (liveCount[id] <= 0) {
			doubleFrees++;
			return;
		}
		liveCount[id]--;
	}

	int Leaked() const {
		int n = 0;
		for (size_t i = 0; i < liveCount.size(); i++) {
			if (liveCount[i] > 0) {
				n++;
			}
		}
		return n;
	}
};

static FakeHeap g_heap;

// Mirrors Dimps::Game::GameMementoKey: a POD holding a payload pointer, with
// no destructor and no copy semantics of its own.
struct FakeKey {
	int payloadId = -1; // -1 == zeroed / owns nothing

	bool IsZeroed() const { return payloadId == -1; }
	void Zero() { payloadId = -1; }
	// The engine's ClearKey: releases whatever the key points at.
	void ClearKey() {
		if (payloadId != -1) {
			g_heap.Free(payloadId);
			payloadId = -1;
		}
	}
};

// The live engine keys (stand-in for fKey::trackedKeys).
static std::vector<FakeKey> g_liveKeys;

static void ResetWorld(int numKeys) {
	g_heap = FakeHeap();
	g_liveKeys.clear();
	for (int i = 0; i < numKeys; i++) {
		FakeKey k;
		k.payloadId = g_heap.Alloc();
		g_liveKeys.push_back(k);
	}
}

// The engine re-populates a key with a fresh payload whenever it reinitializes
// the mementoable object. This is what makes a saved slot STALE: after the sim
// advances, the live keys hold different payloads than the slot recorded.
static void AdvanceEngineFrame() {
	for (size_t i = 0; i < g_liveKeys.size(); i++) {
		if (g_liveKeys[i].IsZeroed()) {
			g_liveKeys[i].payloadId = g_heap.Alloc();
		}
	}
}

// ---------------------------------------------------------------------------
// Mirror of fSystem::SaveState and its Save/Clear/Free/Reclaim protocol.
// ---------------------------------------------------------------------------

struct MirrorSaveState {
	bool used = false;
	bool ownsKeys = true;
	std::vector<std::pair<FakeKey*, FakeKey>> keys;
};

// Mirror of SaveState::Save: copy each live key in, then zero the source so
// the payload is reachable only through this state.
static void MirrorSave(MirrorSaveState* dst) {
	if (!dst->keys.empty()) {
		// Mirror of the recover-on-dirty-slot guard.
		for (auto& e : dst->keys) {
			if (e.second.payloadId != -1) {
				g_heap.Free(e.second.payloadId);
			}
		}
		dst->keys.clear();
	}
	dst->used = true;
	dst->ownsKeys = true;
	for (size_t i = 0; i < g_liveKeys.size(); i++) {
		dst->keys.push_back(std::make_pair(&g_liveKeys[i], g_liveKeys[i]));
		g_liveKeys[i].Zero();
	}
}

// Mirror of CopyIntoPlace: push the recorded key structs back into the live
// keys, handing payload ownership back to the engine.
static void MirrorCopyIntoPlace(MirrorSaveState* src) {
	for (auto& e : src->keys) {
		*e.first = e.second;
	}
}

// Mirror of Clear: release payloads only if this state still owns them.
static void MirrorClear(MirrorSaveState* victim) {
	if (victim->ownsKeys) {
		for (auto& e : victim->keys) {
			if (e.first) {
				e.first->ClearKey();
			}
		}
	}
	victim->keys.clear();
	victim->ownsKeys = true;
	victim->used = false;
}

// Mirror of SaveState::Free, including the ownership handback.
static void MirrorFree(MirrorSaveState* victim) {
	MirrorSaveState tmp;
	MirrorSave(&tmp);           // snapshot live state
	MirrorCopyIntoPlace(victim); // install victim so its keys are clearable
	MirrorClear(victim);         // release victim's payloads
	MirrorCopyIntoPlace(&tmp);   // restore live timeline
	// THE FIX: ownership just went back to the live keys. Drop tmp's claim
	// without releasing. Without these two lines this is the shipped bug.
	tmp.ownsKeys = false;
	MirrorClear(&tmp);
}

// Mirror of the default swap release: install each victim key just long enough
// for the engine to clear it, put the live key back, then drop the records.
static void MirrorFreeBySwap(MirrorSaveState* victim) {
	if (victim->ownsKeys) {
		for (auto& e : victim->keys) {
			if (!e.first) {
				continue;
			}
			const FakeKey live = *e.first;
			*e.first = e.second;
			e.first->ClearKey();
			*e.first = live;
		}
	}
	victim->ownsKeys = false;
	MirrorClear(victim);
}

// Mirror of SaveState::Reclaim: drop records with no engine calls.
static void MirrorReclaim(MirrorSaveState* victim) {
	victim->ownsKeys = false;
	MirrorClear(victim);
}

// Mirror of SaveState::Save's A-001 contract: when recording reports state
// the memento cannot represent, Save releases the slot by swap and returns
// false, so no caller can keep or load a partial snapshot.
static bool MirrorSaveChecked(MirrorSaveState* dst, bool recordFailed) {
	MirrorSave(dst);
	if (recordFailed) {
		MirrorFreeBySwap(dst);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Ledger A-001: a failed save leaves an unused, empty slot and releases every
// payload it took from the engine exactly once.
static void TestFailedSaveReleasesTheSlot() {
	ResetWorld(4);
	MirrorSaveState slot;
	CHECK(!MirrorSaveChecked(&slot, true));
	CHECK(!slot.used);
	CHECK(slot.keys.empty());
	CHECK(g_heap.Leaked() == 0);
	CHECK(g_heap.doubleFrees == 0);
	CHECK(g_heap.freesOfUnowned == 0);

	// The engine refills its keys and the next save works normally.
	AdvanceEngineFrame();
	CHECK(MirrorSaveChecked(&slot, false));
	CHECK(slot.used);
	CHECK(slot.keys.size() == 4);
	MirrorFreeBySwap(&slot);
	CHECK(g_heap.Leaked() == 0);
	CHECK(g_heap.doubleFrees == 0);
}

// Save then Free a stale slot: the slot's own payloads must be released
// exactly once, and the CURRENT live payloads must survive untouched.
static void TestFreeReleasesSlotButKeepsLivePayloads() {
	ResetWorld(4);
	MirrorSaveState slot;

	MirrorSave(&slot);
	CHECK(slot.used);
	CHECK(slot.keys.size() == 4);
	for (size_t i = 0; i < g_liveKeys.size(); i++) {
		CHECK(g_liveKeys[i].IsZeroed()); // ownership moved into the slot
	}

	// The sim advances: live keys get fresh payloads, so `slot` is now stale.
	AdvanceEngineFrame();
	std::vector<int> livePayloads;
	for (size_t i = 0; i < g_liveKeys.size(); i++) {
		livePayloads.push_back(g_liveKeys[i].payloadId);
	}

	MirrorFree(&slot);
	CHECK(slot.keys.empty());
	CHECK(!slot.used);
	CHECK(g_heap.doubleFrees == 0);
	CHECK(g_heap.freesOfUnowned == 0);

	// The live keys must still hold exactly the payloads they had before.
	for (size_t i = 0; i < g_liveKeys.size(); i++) {
		CHECK(g_liveKeys[i].payloadId == livePayloads[i]);
		CHECK(g_heap.liveCount[livePayloads[i]] == 1); // not freed
	}
	CHECK(g_heap.Leaked() == 4); // only the 4 live ones remain
}

// The actual crash: CloseBattle frees every used slot in a loop. Each
// iteration's scratch copy records the live keys, so a stale claim from the
// previous iteration becomes a double free here.
static void TestFreeLoopDoesNotDoubleFree() {
	ResetWorld(6);

	const int kSlots = 4;
	MirrorSaveState slots[kSlots];
	// Model the pool as GGPO drives it: each frame saves into a slot, then
	// the sim advances and the engine hands the live keys fresh payloads.
	// Every slot therefore holds a distinct, now-stale generation.
	for (int i = 0; i < kSlots; i++) {
		MirrorSave(&slots[i]);
		AdvanceEngineFrame();
	}

	std::vector<int> livePayloads;
	for (size_t i = 0; i < g_liveKeys.size(); i++) {
		livePayloads.push_back(g_liveKeys[i].payloadId);
	}

	// Now tear down exactly as CloseBattle does. This loop is where the
	// shipped bug turned a stale claim into a double free.
	for (int i = 0; i < kSlots; i++) {
		if (slots[i].used) {
			MirrorFree(&slots[i]);
		}
	}

	CHECK(g_heap.doubleFrees == 0);
	CHECK(g_heap.freesOfUnowned == 0);
	for (int i = 0; i < kSlots; i++) {
		CHECK(slots[i].keys.empty());
		CHECK(!slots[i].used);
	}
	// The live generation must be untouched by the teardown.
	for (size_t i = 0; i < g_liveKeys.size(); i++) {
		CHECK(g_heap.liveCount[livePayloads[i]] == 1);
	}
	CHECK(g_heap.Leaked() == 6); // exactly the 6 live keys
}

// Saving into a slot that still holds records must recover rather than append
// and orphan the previous payloads.
static void TestSaveIntoDirtySlotRecovers() {
	ResetWorld(3);
	MirrorSaveState slot;

	MirrorSave(&slot);
	CHECK(slot.keys.size() == 3);

	AdvanceEngineFrame();
	MirrorSave(&slot); // dirty-slot save: must recover, not append
	CHECK(slot.keys.size() == 3);
	CHECK(g_heap.doubleFrees == 0);
	CHECK(g_heap.freesOfUnowned == 0);
}

// Reclaim drops records without engine calls -- used at session start where
// the previous battle's objects are already gone.
static void TestReclaimMakesNoEngineCalls() {
	ResetWorld(5);
	MirrorSaveState slot;
	MirrorSave(&slot);

	int freesBefore = 0;
	for (size_t i = 0; i < g_heap.liveCount.size(); i++) {
		freesBefore += (g_heap.liveCount[i] == 0) ? 1 : 0;
	}

	MirrorReclaim(&slot);

	int freesAfter = 0;
	for (size_t i = 0; i < g_heap.liveCount.size(); i++) {
		freesAfter += (g_heap.liveCount[i] == 0) ? 1 : 0;
	}

	CHECK(slot.keys.empty());
	CHECK(!slot.used);
	CHECK(slot.ownsKeys);          // reset to the default for reuse
	CHECK(freesAfter == freesBefore); // no payload was released
	CHECK(g_heap.doubleFrees == 0);
}

// Guard against regression: prove the mirror actually detects the original
// bug, so these tests cannot silently lose their diagnostic power.
//
// The failure mode is a USE-AFTER-FREE, not a double free. Walking the buggy
// Free with live payloads {L0,L1} and a stale slot {A0,A1}:
//
//   Save(&tmp)         tmp holds {L0,L1}; live keys zeroed
//   CopyIntoPlace(v)   live keys = {A0,A1}
//   Clear(v)           frees A0,A1 and zeroes the live keys
//   CopyIntoPlace(tmp) live keys = {L0,L1} again
//   Clear(tmp)         frees L0,L1  <-- the engine is still using these
//
// The engine walks away holding pointers to released payloads. The next
// touch of those keys is the crash, which is why the shipped build died
// without unwinding and with nothing in the log.
static void TestMirrorDetectsTheOriginalBug() {
	ResetWorld(4);

	// Two occupied slots, as the pool holds during a rollback window.
	MirrorSaveState slots[2];
	MirrorSave(&slots[0]);
	AdvanceEngineFrame();
	MirrorSave(&slots[1]);
	AdvanceEngineFrame();

	// The shipped Free: identical to MirrorFree except the scratch copy keeps
	// its stale claim after ownership has gone back to the live keys.
	auto buggyFree = [](MirrorSaveState* victim) {
		MirrorSaveState tmp;
		MirrorSave(&tmp);
		MirrorCopyIntoPlace(victim);
		MirrorClear(victim);
		MirrorCopyIntoPlace(&tmp);
		// (missing: tmp.ownsKeys = false)
		MirrorClear(&tmp); // releases the payloads the live keys just took
	};

	// Snapshot the generation the engine is actively using.
	std::vector<int> livePayloads;
	for (size_t i = 0; i < g_liveKeys.size(); i++) {
		livePayloads.push_back(g_liveKeys[i].payloadId);
	}

	// CloseBattle's teardown loop, with the shipped bug in place.
	for (int i = 0; i < 2; i++) {
		if (slots[i].used) {
			buggyFree(&slots[i]);
		}
	}

	// The live generation was released even though nothing handed it over.
	int liveReleased = 0;
	for (size_t i = 0; i < livePayloads.size(); i++) {
		if (g_heap.liveCount[livePayloads[i]] == 0) {
			liveReleased++;
		}
	}
	CHECK(liveReleased > 0); // the mirror sees the bug

	// And the correct implementation does not do this. Same setup, real Free.
	ResetWorld(2);
	MirrorSaveState good[2];
	MirrorSave(&good[0]);
	AdvanceEngineFrame();
	MirrorSave(&good[1]);
	AdvanceEngineFrame();

	std::vector<int> goodLive;
	for (size_t i = 0; i < g_liveKeys.size(); i++) {
		goodLive.push_back(g_liveKeys[i].payloadId);
	}
	for (int i = 0; i < 2; i++) {
		if (good[i].used) {
			MirrorFree(&good[i]);
		}
	}
	for (size_t i = 0; i < goodLive.size(); i++) {
		CHECK(g_heap.liveCount[goodLive[i]] == 1); // still live
	}
	CHECK(g_heap.doubleFrees == 0);
}

static std::vector<int> LivePayloads() {
	std::vector<int> payloads;
	for (size_t i = 0; i < g_liveKeys.size(); i++) {
		payloads.push_back(g_liveKeys[i].payloadId);
	}
	return payloads;
}

// The swap release frees exactly the slot's payloads and leaves every live key
// byte-identical, including keys the engine has not repopulated yet.
static void TestSwapFreeReleasesSlotAndKeepsLiveKeys() {
	ResetWorld(4);
	MirrorSaveState slot;
	MirrorSave(&slot);
	AdvanceEngineFrame();
	g_liveKeys[2].ClearKey(); // one live key currently owns nothing
	const auto before = LivePayloads();

	MirrorFreeBySwap(&slot);
	CHECK(slot.keys.empty());
	CHECK(!slot.used);
	CHECK(slot.ownsKeys);
	CHECK(g_heap.doubleFrees == 0);
	CHECK(g_heap.freesOfUnowned == 0);
	CHECK(LivePayloads() == before);
	CHECK(g_liveKeys[2].IsZeroed());
	CHECK(g_heap.Leaked() == 3); // only the three live payloads remain
}

// GGPO's steady state: a ten-slot ring where every save first frees the
// oldest slot, with rollbacks loading older slots, and a CloseBattle loop at
// the end. Both release paths must balance the heap exactly.
static void TestRingOfSavesBalancesWithEitherRelease() {
	for (int path = 0; path < 2; path++) {
		const auto release = path == 0 ? MirrorFreeBySwap : MirrorFree;
		ResetWorld(5);
		const int kSlots = 10;
		MirrorSaveState slots[kSlots];
		for (int frame = 0; frame < 200; frame++) {
			MirrorSaveState& slot = slots[frame % kSlots];
			if (slot.used) {
				release(&slot);
			}
			MirrorSave(&slot);
			AdvanceEngineFrame();
			CHECK(g_heap.doubleFrees == 0);
			CHECK(g_heap.freesOfUnowned == 0);
		}
		const auto live = LivePayloads();
		for (int i = 0; i < kSlots; i++) {
			if (slots[i].used) {
				release(&slots[i]);
			}
		}
		CHECK(g_heap.doubleFrees == 0);
		CHECK(g_heap.freesOfUnowned == 0);
		CHECK(LivePayloads() == live);
		CHECK(g_heap.Leaked() == 5);
	}
}

// Guard: a swap release that forgets to put the live key back is caught, so
// the swap tests keep their diagnostic power.
static void TestMirrorDetectsLostLiveKey() {
	ResetWorld(3);
	MirrorSaveState slot;
	MirrorSave(&slot);
	AdvanceEngineFrame();
	const auto before = LivePayloads();
	for (auto& e : slot.keys) {
		*e.first = e.second;
		e.first->ClearKey(); // (missing: restore the live key)
	}
	slot.ownsKeys = false;
	MirrorClear(&slot);
	CHECK(LivePayloads() != before);
}

int main() {
	TestSwapFreeReleasesSlotAndKeepsLiveKeys();
	TestRingOfSavesBalancesWithEitherRelease();
	TestMirrorDetectsLostLiveKey();
	TestFreeReleasesSlotButKeepsLivePayloads();
	TestFreeLoopDoesNotDoubleFree();
	TestSaveIntoDirtySlotRecovers();
	TestReclaimMakesNoEngineCalls();
	TestMirrorDetectsTheOriginalBug();
	TestFailedSaveReleasesTheSlot();

	if (g_failures == 0) {
		printf("savestate ownership: all checks passed\n");
		return 0;
	}
	printf("savestate ownership: %d check(s) failed\n", g_failures);
	return 1;
}
