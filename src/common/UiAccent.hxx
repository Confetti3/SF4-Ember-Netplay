#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace sf4e { namespace accent {
constexpr unsigned Default = 0xFF8738;
constexpr unsigned Maximum = 0xFFFFFF;
inline unsigned Normalize(unsigned rgb) { return rgb <= Maximum ? rgb : Default; }
inline std::string Hex(unsigned rgb) {
    char text[8];
    std::snprintf(text, sizeof(text), "#%06X", Normalize(rgb));
    return text;
}
inline bool Parse(const std::string& text, unsigned& rgb) {
    const std::size_t first = !text.empty() && text[0] == '#' ? 1 : 0;
    if (text.size() - first != 6) return false;
    unsigned value = 0;
    for (std::size_t i = first; i < text.size(); ++i) {
        const char c = text[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        value = (value << 4) | static_cast<unsigned>(digit);
    }
    rgb = value;
    return true;
}
inline int Channel(unsigned rgb, unsigned shift) { return (rgb >> shift) & 255; }
inline unsigned Adjust(unsigned rgb, unsigned shift, int delta) {
    const auto value = (std::max)(0LL, (std::min)(255LL, static_cast<long long>(Channel(rgb, shift)) + delta));
    return (rgb & ~(255u << shift)) | (static_cast<unsigned>(value) << shift);
}
} }
