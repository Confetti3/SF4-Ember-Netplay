#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <string>
#include <vector>
#include <limits>

namespace sf4e { namespace coordination {
inline std::string Sha256(const std::string& bytes) {
    if (bytes.size() > (std::numeric_limits<ULONG>::max)()) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    unsigned char digest[32] = {};
    const auto status = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()),
        digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return {};
    const char* hex = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (auto value : digest) { result += hex[value >> 4]; result += hex[value & 15]; }
    return result;
}
inline std::string Encode(const std::string& bytes) {
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result; result.reserve((bytes.size() * 4 + 2) / 3);
    std::uint32_t bits = 0; unsigned count = 0;
    for (unsigned char value : bytes) {
        bits = (bits << 8) | value; count += 8;
        while (count >= 6) { count -= 6; result += alphabet[(bits >> count) & 63]; }
    }
    if (count) result += alphabet[(bits << (6 - count)) & 63];
    return result;
}
inline bool Decode(const std::string& text, std::string& bytes, std::size_t maximum) {
    if (text.size() > (maximum * 4 + 2) / 3 || text.size() % 4 == 1) return false;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string decoded; decoded.reserve(text.size() * 3 / 4);
    std::uint32_t bits = 0; unsigned count = 0;
    for (char value : text) {
        const auto index = alphabet.find(value);
        if (index == std::string::npos) return false;
        bits = (bits << 6) | static_cast<std::uint32_t>(index); count += 6;
        if (count >= 8) {
            count -= 8;
            if (decoded.size() >= maximum) return false;
            decoded += static_cast<char>((bits >> count) & 255);
        }
    }
    if (count && (bits & ((1u << count) - 1))) return false;
    bytes = std::move(decoded); return true;
}
} }
