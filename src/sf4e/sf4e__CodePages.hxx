#pragma once
#include <windows.h>
#include <cstdint>

namespace sf4e {

// code_edit's access to the game's code (common/CodeEdit.hxx). A protection
// that cannot be put back leaves the bytes writable; they are already final.
struct CodePages {
    bool Unlock(std::uint8_t* at, std::uint8_t length, unsigned long& saved) {
        DWORD old = 0;
        if (!VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &old)) return false;
        saved = old;
        return true;
    }
    void Lock(std::uint8_t* at, std::uint8_t length, unsigned long saved) {
        DWORD old = 0;
        VirtualProtect(at, length, saved, &old);
        FlushInstructionCache(GetCurrentProcess(), at, length);
    }
};

}
