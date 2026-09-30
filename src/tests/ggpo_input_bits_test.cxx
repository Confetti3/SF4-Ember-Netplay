// GGPO decodes input bits into GameInput's fixed buffer. The check it runs
// first (input-bits.h) must pass every stream GGPO's encoder writes and refuse
// any that would read past the declared bits or past the input.
#include "../../vcpkg-overlays/ports/ggpo/input-bits.h"
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
constexpr int BufferBytes = 4096; // UdpMsg's bit buffer
constexpr int MaxInputBytes = 18; // GameInput's bits
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
// Writes bits the way GGPO's BitVector does: least significant first.
struct Writer {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(BufferBytes, 0);
    int offset = 0;
    void Bit(int value) {
        if (value) bytes[offset / 8] |= std::uint8_t(1 << (offset % 8));
        ++offset;
    }
    void Bits(int value, int count) { for (int i = 0; i < count; ++i) Bit((value >> i) & 1); }
    void Raw(int inputBytes) { Bit(0); Bits(0x5A, inputBytes * 8 > 8 ? 8 : inputBytes * 8); for (int i = 8; i < inputBytes * 8; ++i) Bit(i & 1); }
    void Delta(std::initializer_list<int> buttons) {
        Bit(1);
        for (int button : buttons) { Bit(1); Bit(1); Bits(button, 8); }
        Bit(0);
    }
    bool Valid(int inputSize, int numBits = -1) const {
        return sf4e::netplay::InputBitsValid(bytes.data(), numBits < 0 ? offset : numBits, BufferBytes, inputSize, MaxInputBytes);
    }
};
}

int main() {
    // Encoder output: a raw frame, then deltas, for one player's 9 bytes and
    // for the spectator stream's two players (18 bytes).
    for (int size : {9, 18}) {
        Writer w; w.Raw(size); w.Delta({0, 5, size * 8 - 1}); w.Delta({}); w.Raw(size);
        Check(w.Valid(size), "An encoded input stream was refused");
    }
    { Writer w; w.Delta({71}); Check(w.Valid(9), "The last button of a 9-byte input was refused"); }

    // Out of range: a button past the input, a size past GameInput, a size of
    // zero, a raw frame cut short, a delta record cut short, and bits
    // claimed beyond the message's buffer.
    { Writer w; w.Delta({72}); Check(!w.Valid(9), "A button past a 9-byte input was accepted"); }
    { Writer w; w.Delta({255}); Check(!w.Valid(18), "Button 255 was accepted"); }
    { Writer w; w.Raw(9); Check(!w.Valid(19), "An input larger than GameInput was accepted"); }
    { Writer w; w.Delta({1}); Check(!w.Valid(0), "An input size of zero was accepted"); }
    { Writer w; w.Raw(9); Check(!w.Valid(9, w.offset - 1), "A raw frame cut short was accepted"); }
    { Writer w; w.Delta({3}); Check(!w.Valid(9, w.offset - 4), "A delta record cut short was accepted"); }
    { Writer w; w.Delta({3}); Check(!w.Valid(9, w.offset - 1), "A delta without its end marker was accepted"); }
    { Writer w; w.Raw(9); Check(!sf4e::netplay::InputBitsValid(w.bytes.data(), BufferBytes * 8 + 1, BufferBytes, 9, MaxInputBytes),
        "Bits beyond the message buffer were accepted"); }
    if (failures) return 1;
    std::printf("GGPO input bits validation passed\n");
    return 0;
}
