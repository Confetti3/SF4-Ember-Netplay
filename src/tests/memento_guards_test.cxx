#include "../common/MementoGuards.hxx"
#include "test_support.hxx"

#include <map>
#include <utility>
#include <vector>

namespace mg = sf4e::memento;

struct Metadata { uint32_t id[2]; void* memento; };
struct Key {
    void* mementoableObject = nullptr;
    Metadata* metadata = nullptr;
    int numMementos = 0;
    void* mementos = nullptr;
    int sizeAllocated = 0;
};
struct Owner {
    std::vector<std::pair<Key*, Key>> keys;
    bool keyFailure = false;
    size_t guardClaims = 0;
};
constexpr auto kInserted = mg::Claimed::Inserted;

static void TestOwnership() {
    mg::PayloadOwners<Owner> owners;
    Key live;
    int payload[3] = {};
    Owner slot, scratch;
    slot.keys.push_back({&live, {}});
    slot.keys[0].second.mementos = payload;
    CHECK(owners.Insert(&slot, 0) == kInserted);
    CHECK(owners.Find(payload)->owner == &slot);
    // A descriptor borrowed by the engine keeps its slot claim until clear.
    live = slot.keys[0].second;
    scratch.keys.push_back({&live, live});
    CHECK(owners.Insert(&scratch, 0) == mg::Claimed::Duplicate);
    owners.Erase(&scratch);
    CHECK(owners.Find(payload)->owner == &slot);
    // Clear/Initialize revoke before native destruction. Repeated clear and
    // later retirement cannot retain a descriptor pointing at freed bytes.
    CHECK(owners.Revoke(live.mementos));
    CHECK(slot.keyFailure && !slot.keys[0].second.mementos);
    CHECK(!owners.Find(payload));
    CHECK(!owners.Revoke(live.mementos));
    owners.Erase(&slot);
    // Reuse the allocation address in a later slot, then drop a stale slot.
    CHECK(owners.Insert(&scratch, 0) == kInserted);
    owners.Erase(&slot);
    CHECK(owners.Find(payload)->owner == &scratch);
    owners.Erase(&scratch);
    CHECK(!owners.Find(payload));
    // Legacy scratch hands its payloads back to live keys before retirement.
    CHECK(owners.Insert(&scratch, 0) == kInserted);
    live = scratch.keys[0].second;
    owners.Erase(&scratch);
    scratch.keys.clear();
    CHECK(live.mementos == payload && !owners.Find(payload));
    // Null engine payloads never enter the registry.
    slot.keys[0].second = {};
    CHECK(owners.Insert(&slot, 0) == kInserted);
    CHECK(!owners.Find(nullptr));
}

static void TestRing() {
    mg::PayloadOwners<Owner> owners;
    Owner slots[10];
    Key live[10];
    int payload[10] = {};
    for (int frame = 0; frame < 100; ++frame) {
        const int i = frame % 10;
        owners.Erase(&slots[i]);
        slots[i].keys.clear();
        Key saved;
        saved.mementos = &payload[i];
        slots[i].keys.push_back({&live[i], saved});
        CHECK(owners.Insert(&slots[i], 0) == kInserted);
        for (int rollback = 0; rollback <= i; ++rollback) {
            live[rollback] = slots[rollback].keys[0].second;
            CHECK(owners.Find(live[rollback].mementos)->owner == &slots[rollback]);
        }
    }
    for (auto& slot : slots) owners.Erase(&slot);
    for (int& value : payload) CHECK(!owners.Find(&value));
}

static void TestDescriptor() {
    constexpr uintptr_t base = 0x0094E328;
    struct Buffer {
        uintptr_t mementos[2];
        Metadata metadata[2];
    } buffer = {{0x11111111, 0x22222222}, {}};
    buffer.metadata[0].memento = &buffer.mementos[0];
    buffer.metadata[1].memento = &buffer.mementos[1];
    int object = 0;
    const Key good = {&object, buffer.metadata, 2, &buffer, sizeof(buffer)};
    CHECK(!mg::ValidateDescriptor(good, base));
    Key key = good;
    key.numMementos = 0;
    CHECK(mg::ValidateDescriptor(key, base));
    key.numMementos = -1;
    CHECK(mg::ValidateDescriptor(key, base));
    key.numMementos = 65;
    CHECK(mg::ValidateDescriptor(key, base));
    key = good; key.metadata = nullptr;
    CHECK(mg::ValidateDescriptor(key, base));
    key = good; key.metadata = reinterpret_cast<Metadata*>(reinterpret_cast<uintptr_t>(&buffer) + sizeof(buffer) - 1);
    CHECK(mg::ValidateDescriptor(key, base));
    key = good; key.sizeAllocated = -1;
    CHECK(mg::ValidateDescriptor(key, base));
    key = good; key.mementoableObject = nullptr;
    CHECK(mg::ValidateDescriptor(key, base));
    buffer.metadata[1].memento = nullptr;
    CHECK(!mg::ValidateDescriptor(good, base));
    buffer.metadata[1].memento = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&buffer) - 1);
    CHECK(mg::ValidateDescriptor(good, base));
    buffer.metadata[1].memento = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&buffer) + sizeof(buffer) - 1);
    CHECK(mg::ValidateDescriptor(good, base));
    buffer.metadata[1].memento = &buffer.mementos[1];
    buffer.mementos[1] = base;
    CHECK(mg::ValidateDescriptor(good, base));
    CHECK(!mg::ValidateDescriptor(Key{}, base));
    Metadata maximum[64] = {};
    key = {&object, maximum, 64, maximum, sizeof(maximum)};
    CHECK(!mg::ValidateDescriptor(key, base));
    key = good; key.mementos = nullptr;
    CHECK(mg::ValidateDescriptor(key, base));
    // ClearKey leaves sizeAllocated behind; a cleared key is not damaged.
    key = Key{}; key.mementoableObject = &object; key.sizeAllocated = sizeof(buffer);
    CHECK(!mg::ValidateDescriptor(key, base));
    // The legacy round trip installs only owners whose every descriptor would
    // also pass release: a null buffer with leftover metadata or count fails.
    buffer.mementos[1] = 0x22222222;
    Owner owner;
    Key live;
    owner.keys.push_back({&live, key});
    owner.keys.push_back({&live, good});
    CHECK(mg::AllReleasable(owner, base));
    owner.keys[0].second.numMementos = 2;
    CHECK(!mg::AllReleasable(owner, base));
    owner.keys[0].second.numMementos = 0;
    owner.keys[0].second.metadata = buffer.metadata;
    CHECK(!mg::AllReleasable(owner, base));
    owner.keys[0].first = nullptr;
    CHECK(mg::AllReleasable(owner, base));
    CHECK(!mg::ContainsBytes((std::numeric_limits<uintptr_t>::max)() - 3, 8,
        (std::numeric_limits<uintptr_t>::max)() - 2, 1));
    CHECK(mg::ContainsBytes(100, 20, 116, 4));
    CHECK(!mg::ContainsBytes(100, 20, 117, 4));
    CHECK(!mg::ContainsBytes(100, 20, 99, 4));
}

// Probe chains must survive deletes in any order, including chains that wrap
// past the end of the table; checked against a reference map.
static void TestTable() {
    constexpr size_t capacity = 64;
    mg::PayloadOwners<Owner, capacity> owners;
    static char arena[4096];
    Owner owner;
    Key live;
    std::map<void*, size_t> reference;
    uint32_t state = 12345;
    const auto next = [&] { state = state * 1103515245u + 12345u; return state >> 8; };
    for (int step = 0; step < 20000; ++step) {
        void* payload = &arena[(next() % 256) * 16];
        if (next() % 3 && reference.size() < capacity / 2) {
            owner.keys.assign(1, {&live, {}});
            owner.keys[0].second.mementos = payload;
            const bool fresh = !reference.count(payload);
            CHECK((owners.Insert(&owner, 0) == kInserted) == fresh);
            if (fresh) reference[payload] = 0;
        } else {
            owners.Erase(&owner, payload);
            reference.erase(payload);
        }
        CHECK(owners.Size() == reference.size());
        for (int i = 0; i < 256; ++i) {
            void* probe = &arena[i * 16];
            CHECK((owners.Find(probe) != nullptr) == (reference.count(probe) != 0));
        }
    }
    CHECK(owner.guardClaims == reference.size());
    // Past half full a claim is refused, so the capture can fail instead of
    // keeping a payload nothing guards.
    mg::PayloadOwners<Owner, 4> small;
    Owner full;
    for (int i = 0; i < 3; ++i) {
        full.keys.push_back({&live, {}});
        full.keys[i].second.mementos = &arena[i * 16];
    }
    CHECK(small.Insert(&full, 0) == kInserted);
    CHECK(small.Insert(&full, 1) == kInserted);
    CHECK(small.Insert(&full, 2) == mg::Claimed::Full);
    CHECK(small.Size() == 2 && full.guardClaims == 2 && !small.Find(&arena[32]));
}

// Retiring an owner whose saved pointers were damaged after capture must
// still drop every claim it holds, or a later lookup would reach a dead
// owner or the wrong entry of a reused slot.
static void TestDamagedRetirement() {
    mg::PayloadOwners<Owner> owners;
    Key live;
    int payload[3] = {};
    Owner slot;
    for (int& p : payload) {
        slot.keys.push_back({&live, {}});
        slot.keys.back().second.mementos = &p;
    }
    for (size_t i = 0; i < slot.keys.size(); ++i) CHECK(owners.Insert(&slot, i) == kInserted);
    CHECK(slot.guardClaims == 3);
    slot.keys[0].second.mementos = nullptr;
    slot.keys[2].second.mementos = &live;
    owners.Erase(&slot);
    CHECK(slot.guardClaims == 0 && owners.Size() == 0);
    for (int& p : payload) CHECK(!owners.Find(&p));
}

int main() {
    TestOwnership();
    TestTable();
    TestDamagedRetirement();
    TestRing();
    TestDescriptor();
    CHECK(mg::ValidTaskCount(0, 96));
    CHECK(mg::ValidTaskCount(96, 96));
    CHECK(!mg::ValidTaskCount(-1, 96));
    CHECK(!mg::ValidTaskCount(97, 96));
    return 0;
}
