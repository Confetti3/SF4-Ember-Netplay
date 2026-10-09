#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <cstring>

namespace sf4e { namespace compatibility {

// Steam app 45760, build 834219, SSFIV.exe version 2.0.0.93908.
// Captured from the owned Steam installation; see development/GAME_COMPATIBILITY.md.
constexpr LONGLONG GameFileSize = 7026688;
constexpr char GameSha256[] = "5d724595a8ab3c6c6d6f4959187f756f5be35bb497e51e5233c4e73b18b0b9eb";

inline DWORD HashFile(HANDLE file, char (&hex)[65]) {
    hex[0] = '\0';
    LARGE_INTEGER start = {};
    if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN)) return GetLastError();
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    DWORD error = ERROR_SUCCESS;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        return ERROR_INVALID_DATA;
    if (BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return ERROR_INVALID_DATA;
    }
    BYTE buffer[16384], digest[32];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL)) { error = GetLastError(); break; }
        if (!read) break;
        if (BCryptHashData(hash, buffer, read, 0) < 0) { error = ERROR_INVALID_DATA; break; }
    }
    if (!error && BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) error = ERROR_INVALID_DATA;
    if (!error) {
        const char* digits = "0123456789abcdef";
        for (unsigned i = 0; i < sizeof(digest); ++i) {
            hex[i * 2] = digits[digest[i] >> 4];
            hex[i * 2 + 1] = digits[digest[i] & 15];
        }
        hex[64] = '\0';
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return error;
}

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
        const DWORD error = HashFile(file_, digest);
        if (error) return error;
        return std::strcmp(digest, GameSha256) == 0 ? ERROR_SUCCESS : ERROR_REVISION_MISMATCH;
    }
};

inline DWORD CheckLoadedGame() {
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(NULL, path, 32768);
    if (!length) return GetLastError();
    if (length >= 32768) return ERROR_INSUFFICIENT_BUFFER;
    GameFile file;
    return file.Open(path);
}

}}
