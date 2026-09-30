#pragma once
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace sf4e {

// A copy of one of the HUD's native queues (announcements, bonus and combo
// notices) kept in a rollback memento. The memento has a fixed number of
// slots and release builds drop asserts, so the bound is checked here, on
// capture and on restore. A queue longer than the slots cannot be recorded
// whole and a stored count outside them cannot be restored; either one is
// reported, never truncated into a state the game was not in. Unused slots
// stay zeroed so equal HUD states record equal bytes.
template <typename T, std::size_t N> struct HudMementoQueue {
    static_assert(std::is_trivially_copyable<T>::value, "memento entries are copied as bytes");
    static constexpr std::size_t Capacity = N;

    T entries[N];
    int count;

    // False when the source holds more than Capacity entries; the memento is
    // then incomplete and must not be used.
    template <typename Iterator> bool Capture(Iterator first, Iterator last) {
        static_assert(sizeof(*first) == sizeof(T), "native entry and memento entry differ in size");
        std::memset(this, 0, sizeof(*this));
        std::size_t held = 0;
        for (; first != last; ++first, ++held) {
            if (held == N) return false;
            std::memcpy(&entries[held], &*first, sizeof(T));
        }
        count = static_cast<int>(held);
        return true;
    }
    bool Valid() const { return count >= 0 && static_cast<std::size_t>(count) <= N; }
    // Empty unless Valid.
    const T* begin() const { return entries; }
    const T* end() const { return entries + (Valid() ? count : 0); }
};

}
