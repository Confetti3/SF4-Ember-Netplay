#include "../platform/NativeSignature.hxx"
#include "test_support.hxx"
#include <limits>

int main() {
    using namespace sf4e::native;
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const auto page = info.dwPageSize;
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(memory);
    const unsigned char signature[] = {0x55, 0x8b, 0xec, 0xc3};
    std::memcpy(memory + page - 2, signature, sizeof(signature));
    CHECK(Accessible(memory, page * 2));
    CHECK(Signature(memory + page - 2, signature, Access::Readable));
    CHECK(!Signature(memory + page - 2, signature)); // Data is not a callable function.
    DWORD previous = 0;
    CHECK(VirtualProtect(memory, page, PAGE_EXECUTE_READ, &previous));
    CHECK(!Signature(memory + page - 2, signature)); // Every spanned page must be executable.
    CHECK(VirtualProtect(memory + page, page, PAGE_EXECUTE_READ, &previous));
    CHECK(Signature(memory + page - 2, signature));
    const unsigned char mismatch[] = {0x55, 0x8b, 0xec, 0x90};
    CHECK(!Signature(memory + page - 2, mismatch));
    CHECK(Signature(reinterpret_cast<std::uintptr_t>(memory), page - 2, signature));
    CHECK(!Signature((std::numeric_limits<std::uintptr_t>::max)(), 1, signature));
    CHECK(!Accessible(nullptr, 1));
    CHECK(!Accessible(memory, 0));
    CHECK(!Accessible(reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)() - 1), 4));
    CHECK(VirtualProtect(memory + page, page, PAGE_NOACCESS, &previous));
    CHECK(!Accessible(memory + page - 2, sizeof(signature)));
    CHECK(!Signature(memory + page - 2, signature));
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    CHECK(!Accessible(memory, 1));
}
