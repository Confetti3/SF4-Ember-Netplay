#pragma once
#include <cstddef>
#include <cstring>
#include <iterator>
#include <type_traits>

namespace sf4e {

// A copy of one of the HUD's native queues (announcements, bonus and combo
// notices) kept in a rollback memento. The memento has a fixed number of
// slots and release builds drop asserts, so the bound is enforced here, on
// capture and again on restore: a longer native queue keeps only its oldest
// Capacity entries, and a damaged stored count can never index past the
// slots. Unused slots stay zeroed so equal HUD states record equal bytes.
template <typename T, std::size_t N> struct HudMementoQueue {
    static_assert(std::is_trivially_copyable<T>::value, "memento entries are copied as bytes");
    static constexpr std::size_t Capacity = N;

    T entries[N];
    int count;

    // Returns how many entries the source held, which exceeds Capacity when
    // the copy was cut short.
    template <typename Iterator> std::size_t Capture(Iterator first, Iterator last) {
        static_assert(sizeof(*first) == sizeof(T), "native entry and memento entry differ in size");
        std::memset(this, 0, sizeof(*this));
        std::size_t held = 0;
        for (; first != last; ++first, ++held)
            if (held < N) std::memcpy(&entries[held], &*first, sizeof(T));
        count = static_cast<int>(held < N ? held : N);
        return held;
    }
    std::size_t Size() const { return count <= 0 ? 0 : static_cast<std::size_t>(count) < N ? static_cast<std::size_t>(count) : N; }
    const T* begin() const { return entries; }
    const T* end() const { return entries + Size(); }
};

}
