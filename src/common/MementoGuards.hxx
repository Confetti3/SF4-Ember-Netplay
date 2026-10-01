#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace sf4e { namespace memento {

// Initialize (0x52FD40) reserves 12 metadata bytes per slot. Native keys
// use one slot; 64 leaves room while bounding damaged descriptors.
constexpr uint64_t kMetadataBytes = 12;
constexpr int64_t kMaxMementos = 64;

inline bool ContainsBytes(uintptr_t buffer, uint64_t size, uintptr_t address, uint64_t bytes) {
    return buffer && size <= (std::numeric_limits<uintptr_t>::max)() - buffer &&
        address >= buffer && uint64_t(address - buffer) <= size &&
        bytes <= size - uint64_t(address - buffer);
}

// The caller still owns the buffer. Check ranges before reading metadata
// or vtables; this cannot establish the lifetime of an arbitrary allocation.
template <class Key>
const char* ValidateDescriptor(const Key& key, uintptr_t baseVtable) {
    // ClearKey zeroes metadata, numMementos and mementos but leaves
    // sizeAllocated, so a cleared key is judged by those three alone.
    if (!key.mementos) {
        return !key.metadata && !key.numMementos ? nullptr : "null buffer with nonempty descriptor";
    }
    if (!key.mementoableObject) return "null mementoable object";
    if (key.numMementos <= 0 || key.numMementos > kMaxMementos) return "memento count out of range";
    if (key.sizeAllocated <= 0) return "allocation size out of range";
    const uintptr_t buffer = reinterpret_cast<uintptr_t>(key.mementos);
    if (!ContainsBytes(buffer, key.sizeAllocated, reinterpret_cast<uintptr_t>(key.metadata),
        uint64_t(key.numMementos) * sizeof(*key.metadata))) return "metadata outside allocation";
    for (int i = 0; i < key.numMementos; ++i) {
        const void* m = key.metadata[i].memento;
        if (!m) continue;
        if (!ContainsBytes(buffer, key.sizeAllocated, reinterpret_cast<uintptr_t>(m), sizeof(uintptr_t)))
            return "memento outside allocation";
        uintptr_t vtable;
        std::memcpy(&vtable, m, sizeof(vtable));
        if (vtable == baseVtable) return "memento destructor already ran";
    }
    return nullptr;
}

// Whether every saved descriptor of an owner passes release validation, as a
// release would judge it. A release that restores before it frees (the
// legacy round trip) checks this first so nothing invalid is installed.
template <class Owner>
bool AllReleasable(const Owner& owner, uintptr_t baseVtable) {
    for (const auto& entry : owner.keys) {
        if (entry.first && ValidateDescriptor(entry.second, baseVtable)) return false;
    }
    return true;
}

// Owners retain their entries until retirement. Indices survive vector
// growth during capture, and a revoked entry cannot later be loaded or freed.
// Every save, load and release consults this, so it is a fixed open-addressing
// table: nothing is allocated after construction. It never fills past half;
// a claim beyond that is refused as Full, and the caller fails the capture.
// Each owner counts its claims (Owner::guardClaims), so retiring an owner
// whose saved descriptors were damaged still finds every claim it holds.
enum class Claimed { Inserted, Duplicate, Full };

template <class Owner, size_t Capacity = 4096>
class PayloadOwners {
    static_assert(Capacity && (Capacity & (Capacity - 1)) == 0, "capacity is a power of two");
public:
    struct Claim { Owner* owner; size_t entry; };

    PayloadOwners() : slots_(Capacity) {}
    Claimed Insert(Owner* owner, size_t entry) {
        void* payload = owner->keys[entry].second.mementos;
        if (!payload) return Claimed::Inserted;
        size_t at;
        if (Locate(payload, at)) return Claimed::Duplicate;
        if (size_ * 2 >= Capacity) return Claimed::Full;
        slots_[at] = Slot{payload, Claim{owner, entry}};
        ++size_;
        ++owner->guardClaims;
        return Claimed::Inserted;
    }
    const Claim* Find(void* payload) const {
        size_t at;
        return Locate(payload, at) ? &slots_[at].claim : nullptr;
    }
    bool Revoke(void* payload) {
        size_t at;
        if (!Locate(payload, at)) return false;
        Owner* owner = slots_[at].claim.owner;
        owner->keys[slots_[at].claim.entry].second = {};
        owner->keyFailure = true;
        Remove(at);
        return true;
    }
    void Erase(Owner* owner, void* payload) {
        size_t at;
        if (Locate(payload, at) && slots_[at].claim.owner == owner) Remove(at);
    }
    // The current descriptors find the claims on a healthy owner. A claim
    // they no longer name (a damaged pointer) is swept from the whole table;
    // Remove only pulls entries back into the slot being examined, so
    // re-examining it before moving on visits every claim.
    void Erase(Owner* owner) {
        for (const auto& entry : owner->keys) Erase(owner, entry.second.mementos);
        for (size_t i = 0; owner->guardClaims && i < Capacity;) {
            if (slots_[i].payload && slots_[i].claim.owner == owner) Remove(i);
            else ++i;
        }
    }
    size_t Size() const { return size_; }

private:
    struct Slot { void* payload = nullptr; Claim claim{}; };

    static size_t Home(const void* payload) {
        const uint64_t hash = uint64_t(reinterpret_cast<uintptr_t>(payload) >> 4) * 0x9E3779B97F4A7C15ull;
        return size_t(hash >> 32) & (Capacity - 1);
    }
    static size_t Next(size_t i) { return (i + 1) & (Capacity - 1); }
    // The claim's slot, or the empty slot where it would go. The table is at
    // most half full, so the probe always ends.
    bool Locate(const void* payload, size_t& at) const {
        if (!payload) return false;
        for (at = Home(payload); slots_[at].payload; at = Next(at)) {
            if (slots_[at].payload == payload) return true;
        }
        return false;
    }
    // Backward-shift deletion keeps every probe chain whole without
    // tombstones: a later entry moves into the hole unless its home lies
    // after the hole.
    void Remove(size_t hole) {
        --slots_[hole].claim.owner->guardClaims;
        slots_[hole] = Slot{};
        --size_;
        for (size_t i = Next(hole); slots_[i].payload; i = Next(i)) {
            const size_t home = Home(slots_[i].payload);
            if (((i - home) & (Capacity - 1)) >= ((i - hole) & (Capacity - 1))) {
                slots_[hole] = slots_[i];
                slots_[i] = Slot{};
                hole = i;
            }
        }
    }

    std::vector<Slot> slots_;
    size_t size_ = 0;
};

inline bool ValidTaskCount(int count, int capacity) { return count >= 0 && count <= capacity; }

} }
