#pragma once
#include <cstddef>
#include <string>

namespace sf4e {
// Overwrites a string that held a secret, then empties it. The whole buffer is
// wiped, not only the current text: a moved-from short string keeps its old
// characters past its new length.
inline void WipeText(std::string& text) {
    text.resize(text.capacity());
    // Volatile stores, so the compiler cannot drop the wipe of a dying string.
    volatile char* bytes = &text[0];
    for (std::size_t i = 0; i < text.size(); ++i) bytes[i] = 0;
    text.clear();
}
inline void WipeText(char* text, std::size_t size) {
    volatile char* bytes = text;
    for (std::size_t i = 0; i < size; ++i) bytes[i] = 0;
}
}
