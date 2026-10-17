#pragma once

#include <windows.h>
#include "../platform/FileSha256.hxx"
#include <cstring>

namespace sf4e { namespace compatibility {

// Steam app 45760, build 834219, SSFIV.exe version 2.0.0.93908.
// Captured from the owned Steam installation; see development/GAME_COMPATIBILITY.md.
constexpr LONGLONG GameFileSize = 7026688;
constexpr char GameSha256[] = "5d724595a8ab3c6c6d6f4959187f756f5be35bb497e51e5233c4e73b18b0b9eb";

// Hold the read handle through process startup, preventing writes or replacing
// the executable between validation and loading. This is a compatibility gate,
// not protection against a hostile process modifying the game's memory.
class GameFile {
    HANDLE file_ = INVALID_HANDLE_VALUE;
public:
    GameFile() = default;
    GameFile(const GameFile&) = delete;
    GameFile& operator=(const GameFile&) = delete;
    ~GameFile() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    DWORD Open(const wchar_t* path) {
        if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
        file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (file_ == INVALID_HANDLE_VALUE) return GetLastError();
        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(file_, &size)) return GetLastError();
        if (size.QuadPart != GameFileSize) return ERROR_REVISION_MISMATCH;
        char digest[65];
        const DWORD error = platform::FileSha256(file_, digest);
        if (error) return error;
        return std::strcmp(digest, GameSha256) == 0 ? ERROR_SUCCESS : ERROR_REVISION_MISMATCH;
    }
};

}}
