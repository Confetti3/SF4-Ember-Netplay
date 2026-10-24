#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace sf4e { namespace native {
enum class Access { Readable, Executable };

// Call while the native owner cannot free or change these pages. This checks
// committed page permissions, not object lifetime or synchronization.
inline bool Accessible(const void* pointer, std::size_t size, Access access = Access::Readable) {
    auto at = reinterpret_cast<std::uintptr_t>(pointer);
    if (!at || !size || size > (std::numeric_limits<std::uintptr_t>::max)() - at) return false;
    const auto end = at + size;
    while (at < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto protection = region.Protect & 0xff;
        const bool executable = protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;
        const bool readable = executable || protection == PAGE_READONLY || protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY;
        if (!readable || (access == Access::Executable && !executable)) return false;
        const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - start) return false;
        const auto next = start + region.RegionSize;
        if (next <= at) return false;
        at = next;
    }
    return true;
}
template<std::size_t N>
bool Signature(const void* pointer, const unsigned char (&bytes)[N], Access access = Access::Executable) {
    return Accessible(pointer, N, access) && std::memcmp(pointer, bytes, N) == 0;
}
template<std::size_t N>
bool Signature(std::uintptr_t base, std::uintptr_t rva, const unsigned char (&bytes)[N],
    Access access = Access::Executable) {
    return base && rva <= (std::numeric_limits<std::uintptr_t>::max)() - base &&
        Signature(reinterpret_cast<const void*>(base + rva), bytes, access);
}
} }
