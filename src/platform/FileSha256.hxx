#pragma once
#include <windows.h>
#include <bcrypt.h>

namespace sf4e { namespace platform {
// Hash from offset zero; borrows (never closes or reopens) this seekable handle.
// The caller retains its sharing/locking policy. Failure leaves the digest empty.
inline DWORD FileSha256(HANDLE file, char (&hex)[65]) {
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

} }
