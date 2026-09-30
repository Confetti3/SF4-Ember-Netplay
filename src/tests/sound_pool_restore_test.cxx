// Rollback restores the game's sound object pool by relinking its fixed
// entries into the saved active and inactive lists. Both lists must come out
// well formed: every link agrees in both directions and no list points into
// the other.
#include "../sf4e/sf4e__Platform.hxx"
#include <cstdio>
#include <set>

namespace {
using Pool = sf4e::Platform::SoundObjectPool<4>;
using Entry = Dimps::Platform::SoundObjectPoolEntry<4>;
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
// Walks one list from head to tail, checking both directions and collecting
// its entries. Returns false on a broken link or a cycle.
bool Walk(Entry* head, Entry* tail, std::set<Entry*>& seen, std::size_t& count) {
    Entry* previous = nullptr; count = 0;
    for (Entry* cursor = head; cursor; previous = cursor, cursor = cursor->next) {
        if (cursor->prev != previous || !seen.insert(cursor).second) return false;
        ++count;
    }
    return previous == tail;
}
Pool::SaveState Saved(std::size_t active, std::size_t inactive) {
    Pool::SaveState state;
    for (std::size_t i = 0; i < active; ++i) state.active.push_back({static_cast<uint32_t>(100 + i), 0, {}});
    for (std::size_t i = 0; i < inactive; ++i) state.inactive.push_back({static_cast<uint32_t>(200 + i), 0, {}});
    return state;
}
void Restore(std::size_t active, std::size_t inactive, const char* what) {
    Entry entries[3] = {};
    Dimps::Platform::SoundObjectPool<4> pool{};
    // Start with every entry inactive: A <-> B <-> C.
    for (int i = 0; i < 3; ++i) {
        entries[i].prev = i ? &entries[i - 1] : nullptr;
        entries[i].next = i < 2 ? &entries[i + 1] : nullptr;
    }
    pool.inactiveHead = &entries[0]; pool.inactiveTail = &entries[2];
    auto state = Saved(active, inactive);
    Pool::Load(&pool, &state);
    std::set<Entry*> seen; std::size_t activeCount = 0, inactiveCount = 0;
    Check(Walk(pool.activeHead, pool.activeTail, seen, activeCount), what);
    Check(Walk(pool.inactiveHead, pool.inactiveTail, seen, inactiveCount), what);
    Check(activeCount + inactiveCount == 3 && activeCount == (active < 3 ? active : 3), what);
    Check(!pool.activeHead || pool.activeHead->handle == 100, what);
}
}

int main() {
    Restore(1, 2, "One active entry left the inactive list linked back into the active list");
    Restore(2, 1, "Two active entries left the inactive list linked back into the active list");
    Restore(0, 3, "An all-inactive restore broke the inactive list");
    Restore(3, 0, "An all-active restore broke the active list");
    Restore(4, 0, "A saved state larger than the pool walked off the inactive list");
    Restore(1, 5, "More saved inactive entries than the pool holds walked off the list");
    if (failures) return 1;
    std::printf("Sound pool restore keeps both lists well formed.\n");
    return 0;
}
