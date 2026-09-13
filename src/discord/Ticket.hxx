#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace sf4e { namespace discord {
// Metadata inspection only. Rust performs full validation and the host checks
// the capability/build. This bounded parser never makes admission decisions.
inline bool TicketMetadata(const std::string& token, std::string& party, std::uint64_t& expires) {
    if (token.size() != 127 || (token.compare(0, 5, "emd1:") != 0 && token.compare(0,5,"emd2:") != 0)) return false;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::array<unsigned char, 91> bytes{};
    unsigned accumulator = 0, bits = 0; size_t count = 0;
    for (size_t i = 5; i < token.size(); ++i) {
        const auto value = alphabet.find(token[i]);
        if (value == std::string::npos) return false;
        accumulator = (accumulator << 6) | static_cast<unsigned>(value); bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (count >= bytes.size()) return false;
            bytes[count++] = static_cast<unsigned char>(accumulator >> bits);
        }
    }
    if (count != bytes.size() || (accumulator & ((1u << bits) - 1)) || bytes[0] != 1 || bytes[1] || bytes[34] >= 4) return false;
    expires = 0;
    for (int i = 0; i < 8; ++i) expires |= std::uint64_t(bytes[83+i]) << (i*8);
    const char* hex = "0123456789abcdef";
    party = "ember:";
    for (size_t i = 35; i < 51; ++i) { party += hex[bytes[i] >> 4]; party += hex[bytes[i] & 15]; }
    return expires != 0;
}
} }
