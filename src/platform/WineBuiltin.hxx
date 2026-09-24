#pragma once
#include <windows.h>
#include <cstring>
#include <initializer_list>

namespace sf4e { namespace platform {
// Wine ships its own implementation of Microsoft runtime DLLs and marks each
// in the PE stub at offset 0x40. Its version resource borrows a Microsoft
// number (Wine 11's msvcp140 reports 14.42), which says nothing about the
// Microsoft runtime bugs that number would imply. Older prefixes hold
// placeholder stubs with a similar marker; the code then lives in Wine itself.
inline bool IsWineBuiltinDll(const wchar_t* path) {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char header[0x60] = {};
    DWORD read = 0;
    const bool complete = ReadFile(file, header, sizeof(header), &read, nullptr) && read == sizeof(header);
    CloseHandle(file);
    if (!complete || header[0] != 'M' || header[1] != 'Z') return false;
    for (const char* marker : {"Wine builtin DLL", "Wine placeholder DLL"})
        if (std::memcmp(header + 0x40, marker, std::strlen(marker)) == 0) return true;
    return false;
}
} }
