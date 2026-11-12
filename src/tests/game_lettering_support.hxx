#pragma once
// A font library made for the tests (GameLettering.hxx): a DefineFont3 whose
// every glyph is a box, as game_lettering_test and selection_art_test read it.
#include <cstdint>
#include <string>
#include <vector>

namespace lettering_support {
// SWF's bit fields, most significant bit first.
struct BitWriter {
    std::vector<unsigned char> bytes;
    int used = 8;
    void U(std::uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            if (used == 8) { bytes.push_back(0); used = 0; }
            if ((value >> i) & 1u) bytes.back() |= static_cast<unsigned char>(0x80 >> used);
            ++used;
        }
    }
    void S(std::int32_t value, int count) { U(static_cast<std::uint32_t>(value) & ((1u << count) - 1), count); }
};
inline void Put16(std::vector<unsigned char>& out, int value) { out.push_back(value & 0xff); out.push_back((value >> 8) & 0xff); }
// A glyph's shape: a box from (x0, y0) to (x1, y1), y down, as one fill.
inline std::vector<unsigned char> BoxShape(int x0, int y0, int x1, int y1) {
    constexpr int Bits = 16;
    BitWriter w;
    w.U(1, 4); w.U(0, 4);                                    // one fill bit, no line bits
    w.U(0, 1); w.U(0b00101, 5); w.U(Bits, 5); w.S(x0, Bits); w.S(y0, Bits); w.U(1, 1);  // move to the corner, fill 1
    const auto line = [&](bool vertical, int delta) { w.U(1, 1); w.U(1, 1); w.U(Bits - 2, 4); w.U(0, 1); w.U(vertical ? 1 : 0, 1); w.S(delta, Bits); };
    line(false, x1 - x0); line(true, y1 - y0); line(false, x0 - x1); line(true, y0 - y1);
    w.U(0, 1); w.U(0, 5);                                    // the end
    return w.bytes;
}
// A library's tags: a DefineFont3 named `name` with `characters`, each a box
// 0.6 em wide and 0.75 em tall on the baseline (the F's capital height), then
// the end. With `cut`, the last glyph's shape stops halfway.
inline std::vector<unsigned char> Library(const std::string& name, const std::string& characters, bool cut = false) {
    std::vector<std::vector<unsigned char>> shapes;
    for (std::size_t i = 0; i < characters.size(); ++i) {
        auto shape = BoxShape(1000, -15360, 13000, 0);
        if (cut && i + 1 == characters.size()) shape.resize(shape.size() / 2);
        shapes.push_back(shape);
    }
    std::vector<unsigned char> font;
    Put16(font, 1); font.push_back(0x84); font.push_back(1);
    font.push_back(static_cast<unsigned char>(name.size() + 1));
    font.insert(font.end(), name.begin(), name.end()); font.push_back(0);
    Put16(font, static_cast<int>(characters.size()));
    int offset = static_cast<int>(2 * (characters.size() + 1));
    for (const auto& shape : shapes) { Put16(font, offset); offset += static_cast<int>(shape.size()); }
    Put16(font, offset);
    for (const auto& shape : shapes) font.insert(font.end(), shape.begin(), shape.end());
    for (const char c : characters) Put16(font, static_cast<unsigned char>(c));
    Put16(font, 18000); Put16(font, 4000); Put16(font, 2000);
    for (std::size_t i = 0; i < characters.size(); ++i) Put16(font, 14000);
    Put16(font, 0);
    std::vector<unsigned char> tags{0x00, 0, 24, 1, 0};      // an empty frame rectangle, the frame rate and count
    Put16(tags, (75 << 6) | 63);
    tags.push_back(font.size() & 0xff); tags.push_back((font.size() >> 8) & 0xff); tags.push_back((font.size() >> 16) & 0xff); tags.push_back(0);
    tags.insert(tags.end(), font.begin(), font.end());
    Put16(tags, 0);
    return tags;
}
}
