// The HUD's rollback mementos keep a fixed number of queued announcements and
// notices. Release builds drop asserts, so a fifth announcement used to write
// over the stored count and the active control beside the array, and restore
// then installed that garbage. HudMementoQueue refuses what it cannot hold:
// capture reports a queue longer than its slots, and a damaged stored count is
// invalid rather than clamped into a HUD the game never had.
#include "../common/HudMementoQueue.hxx"
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>

#include "test_support.hxx"

struct Entry { int a, b, c, d; };
struct Native { std::uint8_t bytes[16]; };

// The queue as the announce memento lays it out: the active control follows it.
struct Announce {
    std::uint32_t before;
    sf4e::HudMementoQueue<Entry, 4> queue;
    void* activeControl;
    std::uint32_t after;
};

static std::deque<Native> NativeQueue(int n) {
    std::deque<Native> queue;
    for (int i = 0; i < n; ++i) {
        Native entry{};
        const Entry value{ i, i + 1, i + 2, i + 3 };
        static_assert(sizeof(entry) == sizeof(value), "layout");
        std::memcpy(&entry, &value, sizeof(value));
        queue.push_back(entry);
    }
    return queue;
}

static void CheckCapture(int held) {
    Announce m;
    std::memset(&m, 0xCD, sizeof(m));
    m.before = 0x11111111; m.after = 0x22222222;
    m.activeControl = reinterpret_cast<void*>(0x1234);
    const auto native = NativeQueue(held);
    const bool complete = m.queue.Capture(native.begin(), native.end());
    CHECK(complete == (held <= 4));
    CHECK(m.before == 0x11111111 && m.after == 0x22222222);
    CHECK(m.activeControl == reinterpret_cast<void*>(0x1234));
    if (!complete) return;
    CHECK(m.queue.Valid() && m.queue.count == held);
    int i = 0;
    for (const auto& entry : m.queue) { CHECK(entry.a == i && entry.d == i + 3); ++i; }
    CHECK(i == held);
    // Unused slots are zero, so equal HUD states record equal bytes.
    for (int slot = held; slot < 4; ++slot) CHECK(m.queue.entries[slot].a == 0 && m.queue.entries[slot].d == 0);
}

int main() {
    for (int held : {0, 1, 4, 5, 64}) CheckCapture(held);

    // A stored count damaged in the memento is invalid and restores nothing.
    sf4e::HudMementoQueue<Entry, 8> stored{};
    for (int bad : {9, 1000, -1, -5}) {
        stored.count = bad;
        CHECK(!stored.Valid() && stored.begin() == stored.end());
    }
    stored.count = 8; CHECK(stored.Valid() && stored.end() == stored.begin() + 8);

    std::cout << "HUD memento bounds passed\n";
    return 0;
}
