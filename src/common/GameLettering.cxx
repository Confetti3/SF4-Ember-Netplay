#include "GameLettering.hxx"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// The font library's tags read into outlines, and the outlines drawn into the
// sheet (GameLettering.hxx).
namespace sf4e { namespace game_lettering {
namespace {
// SWF's bit fields, most significant bit first. A read past the end gives 0 and clears `ok`.
struct Bits {
    const unsigned char* data; std::size_t size, bit = 0;
    bool ok = true;
    std::uint32_t U(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i, ++bit) {
            if ((bit >> 3) >= size) { ok = false; return 0; }
            value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1u);
        }
        return value;
    }
    std::int32_t S(int count) {
        if (count <= 0) return 0;
        const std::uint32_t value = U(count);
        return count < 32 && (value & (1u << (count - 1))) ? static_cast<std::int32_t>(value) - (1 << count) : static_cast<std::int32_t>(value);
    }
};
struct Segment { float x0, y0, x1, y1; };
std::uint32_t Read16(const unsigned char* at) { return at[0] | (at[1] << 8); }
std::uint32_t Read32(const unsigned char* at) { return Read16(at) | (Read16(at + 2) << 16); }
// A glyph's outline (a SHAPE: edges only, one fill), its curves cut into
// eight lines each. False when it does not read.
bool Outline(const unsigned char* data, std::size_t size, std::vector<Segment>& segments) {
    Bits bits{data, size};
    const int fillBits = static_cast<int>(bits.U(4)), lineBits = static_cast<int>(bits.U(4));
    float x = 0, y = 0;
    for (int records = 0; bits.ok && records < 4096; ++records) {
        if (!bits.U(1)) {
            const std::uint32_t flags = bits.U(5);
            if (!flags) return bits.ok;
            if (flags & 16) return false;  // new styles: not a glyph
            if (flags & 1) { const int n = static_cast<int>(bits.U(5)); x = static_cast<float>(bits.S(n)); y = static_cast<float>(bits.S(n)); }
            if (flags & 2) bits.U(fillBits);
            if (flags & 4) bits.U(fillBits);
            if (flags & 8) bits.U(lineBits);
            continue;
        }
        const bool straight = bits.U(1) != 0;
        const int n = static_cast<int>(bits.U(4)) + 2;
        if (straight) {
            float dx = 0, dy = 0;
            if (bits.U(1)) { dx = static_cast<float>(bits.S(n)); dy = static_cast<float>(bits.S(n)); }
            else if (bits.U(1)) dy = static_cast<float>(bits.S(n));
            else dx = static_cast<float>(bits.S(n));
            segments.push_back({x, y, x + dx, y + dy});
            x += dx; y += dy;
            continue;
        }
        const float cx = x + bits.S(n), cy = y + bits.S(n);
        const float ax = cx + bits.S(n), ay = cy + bits.S(n);
        float px = x, py = y;
        for (int i = 1; i <= 8; ++i) {
            const float t = i / 8.f, u = 1 - t;
            const float qx = u * u * x + 2 * u * t * cx + t * t * ax, qy = u * u * y + 2 * u * t * cy + t * t * ay;
            segments.push_back({px, py, qx, qy});
            px = qx; py = qy;
        }
        x = ax; y = ay;
    }
    return false;
}
// Coverage of the outline, even-odd, scaled by `scale` and offset so the
// pixel (left, top) is the bitmap's first, into `alpha` (w by h).
void Fill(const std::vector<Segment>& segments, float scale, int left, int top, int w, int h, unsigned char* alpha, int pitch) {
    std::vector<float> crossings, coverage(static_cast<std::size_t>(w));
    for (int row = 0; row < h; ++row) {
        std::fill(coverage.begin(), coverage.end(), 0.f);
        for (int sub = 0; sub < Subrows; ++sub) {
            const float y = top + row + (sub + .5f) / Subrows;
            crossings.clear();
            for (const auto& s : segments) {
                const float y0 = s.y0 * scale, y1 = s.y1 * scale;
                if ((y0 <= y && y < y1) || (y1 <= y && y < y0)) {
                    const float x0 = s.x0 * scale, x1 = s.x1 * scale;
                    crossings.push_back(x0 + (y - y0) * (x1 - x0) / (y1 - y0) - left);
                }
            }
            std::sort(crossings.begin(), crossings.end());
            for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
                const float a = (std::max)(0.f, crossings[i]), b = (std::min)(static_cast<float>(w), crossings[i + 1]);
                for (int x = static_cast<int>(a); x < w && x < b; ++x)
                    coverage[x] += ((std::min)(x + 1.f, b) - (std::max)(static_cast<float>(x), a)) / Subrows;
            }
        }
        for (int x = 0; x < w; ++x) alpha[row * pitch + x] = static_cast<unsigned char>(std::lround((std::min)(1.f, coverage[x]) * 255));
    }
}

// The font's characters as outlines, with their advances and the capital
// height, read from a font library's tags.
struct Outlines {
    float capTop = 0, capBottom = 0;
    std::vector<Glyph> glyphs;
    std::vector<std::vector<Segment>> outlines;
};
bool ReadOutlines(const unsigned char* swf, std::size_t size, Outlines& result, std::string& why) {
    if (!swf || size < 2) { why = "empty"; return false; }
    // The frame's rectangle, the frame rate and the frame count, then the tags.
    std::size_t at = (5 + 4 * (swf[0] >> 3) + 7) / 8 + 4;
    const unsigned char* font = nullptr;
    std::size_t length = 0;
    while (at + 2 <= size) {
        const std::uint32_t head = Read16(swf + at);
        at += 2;
        const std::uint32_t code = head >> 6;
        std::size_t tag = head & 63;
        if (tag == 63) { if (at + 4 > size) break; tag = Read32(swf + at); at += 4; }
        if (tag > size - at) break;
        const std::size_t nameLength = tag > 5 ? swf[at + 4] : 0;
        if ((code == 75 || code == 48) && 5 + nameLength <= tag && nameLength >= std::strlen(FontName) &&
            std::memcmp(swf + at + 5, FontName, std::strlen(FontName)) == 0 &&
            (nameLength == std::strlen(FontName) || swf[at + 5 + std::strlen(FontName)] == 0)) {
            font = swf + at; length = tag; break;
        }
        if (code == 0) break;
        at += tag;
    }
    if (!font) { why = std::string("no ") + FontName; return false; }
    // DefineFont3: flags, the name, the glyph count, the shapes' offsets and the code table's, the shapes, the codes, the advances.
    const unsigned flags = font[2];
    const bool wideOffsets = (flags & 0x08) != 0, wideCodes = (flags & 0x04) != 0, layout = (flags & 0x80) != 0;
    const std::size_t table = 5 + font[4] + 2, offset = wideOffsets ? 4 : 2, code = wideCodes ? 2 : 1;
    if (table > length) { why = "short font"; return false; }
    const std::size_t count = Read16(font + table - 2);
    const auto offsetAt = [&](std::size_t i) -> std::size_t { return wideOffsets ? Read32(font + table + i * 4) : Read16(font + table + i * 2); };
    if (!layout || !count || table + (count + 1) * offset > length) { why = "no glyph table"; return false; }
    const std::size_t codes = table + offsetAt(count), advances = codes + count * code + 6;
    if (codes > length || advances + count * 2 > length) { why = "no codes or advances"; return false; }
    result = Outlines{};
    for (const char* c = Characters; *c; ++c) {
        std::size_t index = 0;
        while (index < count && (wideCodes ? Read16(font + codes + index * 2) : font[codes + index]) != static_cast<unsigned char>(*c)) ++index;
        if (index == count) { why = std::string("no glyph ") + *c; return false; }
        const std::size_t begin = table + offsetAt(index), end = index + 1 < count ? table + offsetAt(index + 1) : codes;
        std::vector<Segment> outline;
        if (begin >= end || end > length || !Outline(font + begin, end - begin, outline) || outline.empty()) { why = std::string("glyph ") + *c + " does not read"; return false; }
        Glyph glyph;
        glyph.code = *c;
        glyph.advance = static_cast<float>(static_cast<std::int16_t>(Read16(font + advances + index * 2)));
        if (*c == 'F') {
            result.capTop = result.capBottom = outline.front().y0;
            for (const auto& s : outline) { result.capTop = (std::min)({result.capTop, s.y0, s.y1}); result.capBottom = (std::max)({result.capBottom, s.y0, s.y1}); }
        }
        result.glyphs.push_back(glyph);
        result.outlines.push_back(std::move(outline));
    }
    if (!(result.capBottom > result.capTop)) { why = "no capital height"; return false; }
    return true;
}

// The outlines drawn at each size in shelves, the glyphs left to right, as
// white with the coverage as alpha.
bool DrawSheet(const Outlines& font, Sheet& sheet, std::vector<unsigned char>& bgra, std::string& why) {
    sheet = Sheet{};
    sheet.capTop = font.capTop; sheet.capBottom = font.capBottom; sheet.glyphs = font.glyphs;
    bgra.assign(static_cast<std::size_t>(SheetWidth) * SheetHeight * 4, 0);
    std::vector<unsigned char> alpha(static_cast<std::size_t>(SheetWidth) * SheetHeight, 0);
    int x = 0, y = 0, shelf = 0;
    for (int size = 0; size < SizeCount; ++size) {
        const float scale = Sizes[size] / (sheet.capBottom - sheet.capTop);
        sheet.scale[size] = scale;
        for (std::size_t g = 0; g < sheet.glyphs.size(); ++g) {
            float x0 = font.outlines[g].front().x0, x1 = x0, y0 = font.outlines[g].front().y0, y1 = y0;
            for (const auto& s : font.outlines[g]) { x0 = (std::min)({x0, s.x0, s.x1}); x1 = (std::max)({x1, s.x0, s.x1}); y0 = (std::min)({y0, s.y0, s.y1}); y1 = (std::max)({y1, s.y0, s.y1}); }
            // Whole pixels round the ink, a float's error short of a whole one taken as it.
            const auto down = [](float v) { return static_cast<int>(std::floor(v + 1e-3f)); };
            const auto up = [](float v) { return static_cast<int>(std::ceil(v - 1e-3f)); };
            const int left = down(x0 * scale) - Padding, top = down(y0 * scale) - Padding;
            const int w = up(x1 * scale) + Padding - left, h = up(y1 * scale) + Padding - top;
            if (x + w > SheetWidth) { x = 0; y += shelf; shelf = 0; }
            if (w > SheetWidth || y + h > SheetHeight) { why = "the glyphs do not fit the sheet"; return false; }
            Glyph& glyph = sheet.glyphs[g];
            glyph.box[size] = {x, y, w, h};
            glyph.left[size] = left; glyph.top[size] = top;
            Fill(font.outlines[g], scale, left, top, w, h, alpha.data() + y * SheetWidth + x, SheetWidth);
            x += w; shelf = (std::max)(shelf, h);
        }
        x = 0; y += shelf; shelf = 0;
    }
    for (std::size_t i = 0; i < alpha.size(); ++i) {
        bgra[i * 4] = bgra[i * 4 + 1] = bgra[i * 4 + 2] = 255;
        bgra[i * 4 + 3] = alpha[i];
    }
    return true;
}
}

bool Read(const unsigned char* swf, std::size_t size, Sheet& sheet, std::vector<unsigned char>& bgra, std::string& why) {
    Outlines font;
    if (!ReadOutlines(swf, size, font, why)) return false;
    return DrawSheet(font, sheet, bgra, why);
}
} }
