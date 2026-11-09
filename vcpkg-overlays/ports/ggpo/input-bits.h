#pragma once

#include <stdint.h>

namespace sf4e {
namespace netplay {

// GGPO sends each frame's input as one flag bit, then either delta records
// ({more:1, on:1, button:8}, ending with a clear "more" bit) or input_size * 8
// raw bits, least significant bit first. The decoder writes button indices
// straight into GameInput's fixed bits, so the stream is walked once before
// any of it is applied: every read stays inside the declared bits, and every
// button inside the input.
// bufferBytes is the message's bit buffer; maxInputBytes is GameInput's.
// Returns the number of frames the stream holds, or -1 when it is invalid.
inline int InputBitsFrames(const uint8_t* bits, int numBits, int bufferBytes, int inputSize, int maxInputBytes) {
    if (!bits || numBits < 0 || numBits > bufferBytes * 8 || inputSize <= 0 || inputSize > maxInputBytes) {
        return -1;
    }
    const int inputBits = inputSize * 8;
    int offset = 0;
    int frames = 0;
    const auto read = [&](int count, int* value) {
        if (count > numBits - offset) {
            return false;
        }
        int result = 0;
        for (int i = 0; i < count; i++, offset++) {
            result |= ((bits[offset / 8] >> (offset % 8)) & 1) << i;
        }
        *value = result;
        return true;
    };
    while (offset < numBits) {
        int delta = 0;
        if (!read(1, &delta)) {
            return -1;
        }
        frames++;
        if (!delta) {
            if (inputBits > numBits - offset) {
                return -1;
            }
            offset += inputBits;
            continue;
        }
        for (;;) {
            int more = 0, on = 0, button = 0;
            if (!read(1, &more)) {
                return -1;
            }
            if (!more) {
                break;
            }
            if (!read(1, &on) || !read(8, &button) || button >= inputBits) {
                return -1;
            }
        }
    }
    return frames;
}

inline bool InputBitsValid(const uint8_t* bits, int numBits, int bufferBytes, int inputSize, int maxInputBytes) {
    return InputBitsFrames(bits, numBits, bufferBytes, inputSize, maxInputBytes) >= 0;
}

}
}
