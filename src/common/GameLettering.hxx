#pragma once
// The game's own lettering for the points' labels (FP, BP, and a gain's plus):
// SF4 sets its PP and BP in FOT-RodinNTLG Pro B, a Scaleform font whose
// outlines are in resource/ui/fonts/ENG/fonts.gfx of the player's own game,
// read at runtime and never shipped. Its capitals, digits and plus are drawn
// here, white on clear, into one sheet at a few capital heights, so a label is
// drawn from the nearest size at least as tall and never shrunk by more than
// half. A file without that font, or whose outlines do not read, is refused,
// and the labels keep the heading font. Only the reading and drawing are here,
// so they can be checked without a device; SelectionArt::GameLetters uploads
// the sheet and v2::Level draws from it (LevelMark.cxx).
#include <cstddef>
#include <string>
#include <vector>

namespace sf4e { namespace game_lettering {
constexpr const char* FontName = "FOT-RodinNTLG Pro B";
// What the sheet holds.
constexpr const char* Characters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+";
constexpr int SheetWidth = 512, SheetHeight = 512;
// The capital heights drawn, in pixels, and the clear pixels round each glyph.
constexpr int Sizes[] = {16, 32, 64};
constexpr int SizeCount = 3;
constexpr int Padding = 2;
// Rows sampled for each pixel row; across, coverage is exact.
constexpr int Subrows = 4;

struct Box { int x = 0, y = 0, w = 0, h = 0; };
struct Glyph {
    char code = 0;
    // The pen's advance, in the font's units (20480 to the em).
    float advance = 0;
    // At each size: the glyph's box in the sheet, and its top left from the
    // pen on the baseline in that size's pixels (y down).
    Box box[SizeCount];
    int left[SizeCount] = {}, top[SizeCount] = {};
};
struct Sheet {
    // The F's ink from the baseline, y down (capTop is negative), in the font's
    // units: the capital height every size is measured on.
    float capTop = 0, capBottom = 0;
    // Pixels per font unit at each size.
    float scale[SizeCount] = {};
    std::vector<Glyph> glyphs;
    const Glyph* Find(char code) const {
        for (const auto& glyph : glyphs) if (glyph.code == code) return &glyph;
        return nullptr;
    }
    // Whether every character of `text` is in the sheet (and there is one).
    bool Covers(const char* text) const {
        if (!text || !*text) return false;
        for (; *text; ++text) if (!Find(*text)) return false;
        return true;
    }
    // The size to draw capitals `height` tall from: the smallest at least that tall, else the largest.
    static int SizeFor(float height) {
        for (int i = 0; i < SizeCount; ++i) if (Sizes[i] >= height) return i;
        return SizeCount - 1;
    }
};

// Reads the font from a font library's tags (the file after its 8-byte
// header, inflated) and draws the sheet into `bgra` (SheetWidth by
// SheetHeight, 4 bytes a pixel, white with the coverage as alpha). False, with
// `why`, when the font, one of its characters or an outline is not there or
// the glyphs do not fit; `sheet` is then unspecified.
bool Read(const unsigned char* swf, std::size_t size, Sheet& sheet, std::vector<unsigned char>& bgra, std::string& why);
} }
