// The Scaleform sprite-action pool must come back from a rollback with its
// free stack, not only its in-use flags. Restoring the flags alone left an
// action live that the stack would hand out again: two owners of one slot,
// and later two releases of it. The pool below follows SSFIV's allocate
// (0x6EE9A0) and release (0x6ED610) exactly.
#include "../common/GfxActionPoolState.hxx"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "test_support.hxx"

using namespace sf4e::gfx;

struct Action { char pad[0x260]; };

struct Pool {
    void* myData;
    Action* raw;
    Action** free;
    std::uint8_t* useIndex;
    std::uint32_t max, used, highWater, pad;
};

struct Storage {
    std::vector<Action> raw;
    std::vector<Action*> free;
    std::vector<std::uint8_t> useIndex;
    Pool pool{};
    explicit Storage(std::uint32_t max) : raw(max), free(max), useIndex(max) {
        pool.raw = raw.data(); pool.free = free.data(); pool.useIndex = useIndex.data(); pool.max = max;
        for (std::uint32_t i = 0; i < max; ++i) free[i] = &raw[max - 1 - i];
    }
    Action* Allocate() {
        if (pool.used >= pool.max) return nullptr;
        Action* slot = pool.free[pool.used++];
        pool.useIndex[slot - pool.raw] = 1;
        if (pool.highWater < pool.used) pool.highWater = pool.used;
        return slot;
    }
    void Release(Action* slot) {
        if (!slot || !pool.used) return;
        pool.free[--pool.used] = slot;
        pool.useIndex[slot - pool.raw] = 0;
    }
};

static void TestRollbackKeepsOneOwner() {
    Storage s(16);
    Action* a = s.Allocate(); Action* b = s.Allocate();
    auto saved = std::make_unique<PoolState>();
    CHECK(Capture(s.pool, *saved));
    // After the save: a is released and its slot reused, then more churn.
    s.Release(a);
    Action* c = s.Allocate(); CHECK(c == a);
    Action* d = s.Allocate(); s.Release(b); s.Release(d);

    // What the timeline after the save allocates next, from the saved state.
    Storage expected(16);
    expected.Allocate(); expected.Allocate();
    Action* expectedNext = expected.Allocate();

    CHECK(Fits(s.pool, *saved));
    Apply(s.pool, *saved);
    CHECK(s.pool.used == 2 && s.pool.useIndex[a - s.pool.raw] == 1 && s.pool.useIndex[b - s.pool.raw] == 1);
    Action* next = s.Allocate();
    CHECK(next != a && next != b);
    CHECK(next - s.pool.raw == expectedNext - expected.pool.raw);
    // Every live slot is off the free stack.
    for (std::uint32_t i = s.pool.used; i < s.pool.max; ++i) CHECK(!s.pool.useIndex[s.pool.free[i] - s.pool.raw]);
}

static void TestFlagsAloneGaveTwoOwners() {
    // The old restore, for the record: flags back, stack left as it was.
    Storage s(4);
    Action* a = s.Allocate();
    std::vector<std::uint8_t> flags(s.useIndex);
    s.Release(a);
    std::memcpy(s.pool.useIndex, flags.data(), flags.size());
    CHECK(s.Allocate() == a); // handed out while still marked live
}

static void TestEdges() {
    for (std::uint32_t max : {1u, 5u, static_cast<std::uint32_t>(MaxActions)}) {
        Storage s(max);
        auto state = std::make_unique<PoolState>();
        CHECK(Capture(s.pool, *state) && Fits(s.pool, *state)); // empty
        while (s.Allocate()) {}
        CHECK(Capture(s.pool, *state) && Fits(s.pool, *state)); // full
        Apply(s.pool, *state);
        CHECK(s.pool.used == max && !s.Allocate());
    }
    Storage big(static_cast<std::uint32_t>(MaxActions) + 1);
    auto state = std::make_unique<PoolState>();
    CHECK(!Capture(big.pool, *state));
}

static void TestRejectsWithoutWriting() {
    Storage s(8);
    s.Allocate(); s.Allocate(); s.Allocate();
    auto good = std::make_unique<PoolState>();
    CHECK(Capture(s.pool, *good));
    const std::vector<Action*> freeBefore(s.free);
    const std::vector<std::uint8_t> flagsBefore(s.useIndex);
    const auto usedBefore = s.pool.used;
    auto bad = [&](void (*damage)(PoolState&)) {
        auto state = std::make_unique<PoolState>(*good);
        damage(*state);
        CHECK(!Fits(s.pool, *state));
    };
    bad([](PoolState& p) { p.raw = &p; });
    bad([](PoolState& p) { p.max = 7; });
    bad([](PoolState& p) { p.used = 9; });
    bad([](PoolState& p) { p.freeSlots[1] = p.freeSlots[0]; });  // a slot twice
    bad([](PoolState& p) { p.freeSlots[0] = 200; });               // outside the pool
    bad([](PoolState& p) { p.inUse[p.freeSlots[0]] = 1; });        // live and free
    bad([](PoolState& p) { p.inUse[p.freeSlots[0]] = 2; });
    bad([](PoolState& p) { p.used = 2; });                         // count and flags disagree
    Storage other(8);
    CHECK(!Fits(other.pool, *good));                               // different storage
    CHECK(s.free == freeBefore && s.useIndex == flagsBefore && s.pool.used == usedBefore);
}

int main() {
    TestRollbackKeepsOneOwner();
    TestFlagsAloneGaveTwoOwners();
    TestEdges();
    TestRejectsWithoutWriting();
    std::cout << "GFx action pool passed\n";
    return 0;
}
