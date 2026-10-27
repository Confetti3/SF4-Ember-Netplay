#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <limits>
// The dependency-free SHA-256: the digest off Windows, where the room host has
// no CNG, the fallback when CNG fails, and the reference CNG is tested against.
#include "Sha256.hxx"
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace sf4e { namespace coordination {
// Lowercase hex SHA-256, the one digest for checkpoint transfers, committed
// effects (recovery_detail::Sha256) and replay summaries.
#ifndef _WIN32
inline std::string Sha256(const std::string& bytes) { return Sha256Portable(bytes); }
#else
inline std::string Sha256(const std::string& bytes) {
    // One provider handle for the process; CNG algorithm handles may be used
    // from several threads at once (the checkpoint decode worker hashes too).
    static const BCRYPT_ALG_HANDLE algorithm = [] {
        BCRYPT_ALG_HANDLE handle = nullptr;
        return BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 ? handle : nullptr;
    }();
    unsigned char digest[32] = {};
    if (!algorithm || bytes.size() > (std::numeric_limits<ULONG>::max)() ||
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
            static_cast<ULONG>(bytes.size()), digest, sizeof(digest)) < 0) return Sha256Portable(bytes);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (const auto value : digest) { result.push_back(hex[value >> 4]); result.push_back(hex[value & 0xfU]); }
    return result;
}
#endif
// Unpadded base64url, the helper's checkpoint chunk encoding.
inline std::string Encode(const char* data, std::size_t size) {
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result; result.reserve((size * 4 + 2) / 3);
    std::uint32_t bits = 0; unsigned count = 0;
    for (std::size_t i = 0; i < size; ++i) {
        bits = (bits << 8) | static_cast<unsigned char>(data[i]); count += 8;
        while (count >= 6) { count -= 6; result += alphabet[(bits >> count) & 63]; }
    }
    if (count) result += alphabet[(bits << (6 - count)) & 63];
    return result;
}
inline std::string Encode(const std::string& bytes) { return Encode(bytes.data(), bytes.size()); }

namespace bytes_detail {
// The value of each base64url character, 64 for every other byte.
struct DecodeTable {
    unsigned char value[256];
    constexpr DecodeTable() : value() {
        for (int i = 0; i < 256; ++i) value[i] = 64;
        for (int i = 0; i < 26; ++i) { value['A' + i] = static_cast<unsigned char>(i); value['a' + i] = static_cast<unsigned char>(26 + i); }
        for (int i = 0; i < 10; ++i) value['0' + i] = static_cast<unsigned char>(52 + i);
        value[static_cast<unsigned char>('-')] = 62; value[static_cast<unsigned char>('_')] = 63;
    }
};
constexpr DecodeTable Base64Url{};
}

// Decodes unpadded base64url text and appends it to `out`, at most `maximum`
// bytes. Rejects, leaving `out` as it was: text longer than `maximum` bytes
// can encode, a length of 1 modulo 4, a character outside the alphabet, and
// non-zero trailing bits.
inline bool DecodeAppend(const std::string& text, std::string& out, std::size_t maximum) {
    if (text.size() > (maximum * 4 + 2) / 3 || text.size() % 4 == 1) return false;
    const auto original = out.size();
    out.reserve(original + text.size() * 3 / 4);
    std::uint32_t bits = 0; unsigned count = 0;
    std::size_t decoded = 0;
    for (const char value : text) {
        const auto index = bytes_detail::Base64Url.value[static_cast<unsigned char>(value)];
        if (index == 64) { out.resize(original); return false; }
        bits = (bits << 6) | index; count += 6;
        if (count >= 8) {
            count -= 8;
            if (decoded >= maximum) { out.resize(original); return false; }
            out += static_cast<char>((bits >> count) & 255);
            ++decoded;
        }
    }
    if (count && (bits & ((1u << count) - 1))) { out.resize(original); return false; }
    return true;
}
inline bool Decode(const std::string& text, std::string& bytes, std::size_t maximum) {
    std::string decoded;
    if (!DecodeAppend(text, decoded, maximum)) return false;
    bytes = std::move(decoded); return true;
}
} }
