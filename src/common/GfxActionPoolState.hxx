#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sf4e { namespace gfx {

// The allocator state of the game's Scaleform sprite-action pool, kept in a
// rollback memento beside the actions themselves. SSFIV's pool (allocate at
// 0x6EE9A0, release at 0x6ED610) is a stack of free slot pointers:
//   allocate: slot = free[used]; ++used; inUse[slot] = 1
//   release:  --used; free[used] = slot; inUse[slot] = 0
// so free[used..max) are the free slots in the order they will be handed out,
// and free[0..used) is never read. Restoring the in-use flags without the
// stack left a slot live that the stack would hand out again, giving one
// action two owners; the stack and its count now travel with the flags.
constexpr std::size_t MaxActions = 0x44c;

struct PoolState {
    const void* raw;
    std::uint32_t max, used;
    std::uint8_t inUse[MaxActions];
    // free[used..max), as slot indices.
    std::uint16_t freeSlots[MaxActions];
};

// Pool is Dimps::Platform::GFxApp::ObjectPool<T>: raw, free, useIndex, max, used.
// False when the live pool breaks the model above; the memento is then unusable.
template <typename Pool> bool Capture(const Pool& pool, PoolState& state) {
    std::memset(&state, 0, sizeof(state));
    if (!pool.raw || !pool.free || !pool.useIndex || pool.max > MaxActions || pool.used > pool.max) return false;
    state.raw = pool.raw; state.max = pool.max; state.used = pool.used;
    std::memcpy(state.inUse, pool.useIndex, pool.max);
    for (std::uint32_t i = pool.used; i < pool.max; ++i) {
        const auto slot = pool.free[i] - pool.raw;
        if (slot < 0 || static_cast<std::uint32_t>(slot) >= pool.max) return false;
        state.freeSlots[i - pool.used] = static_cast<std::uint16_t>(slot);
    }
    return true;
}

// The saved state fits this pool and is self-consistent: the same storage, and
// every slot either in use or on the free stack exactly once.
template <typename Pool> bool Fits(const Pool& pool, const PoolState& state) {
    if (state.raw != pool.raw || state.max != pool.max || state.used > state.max || state.max > MaxActions) return false;
    std::uint8_t seen[MaxActions] = {};
    std::uint32_t live = 0;
    for (std::uint32_t slot = 0; slot < state.max; ++slot) {
        if (state.inUse[slot] > 1) return false;
        live += state.inUse[slot];
    }
    if (live != state.used) return false;
    for (std::uint32_t i = 0; i < state.max - state.used; ++i) {
        const auto slot = state.freeSlots[i];
        if (slot >= state.max || state.inUse[slot] || seen[slot]) return false;
        seen[slot] = 1;
    }
    return true;
}

// Only after Fits.
template <typename Pool> void Apply(Pool& pool, const PoolState& state) {
    std::memcpy(pool.useIndex, state.inUse, state.max);
    pool.used = state.used;
    for (std::uint32_t i = 0; i < state.max - state.used; ++i) pool.free[state.used + i] = pool.raw + state.freeSlots[i];
}

} }
