#include "HudTextures.hxx"
#include "HudLayout.hxx"
#include "../common/Inflate.hxx"
#include <zlib.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace sf4e { namespace platform { namespace hud_textures {
namespace {
std::uint32_t U32(const std::string& d, size_t at) { std::uint32_t v = 0; if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4); return v; }
void Colour565(unsigned c, int& r, int& g, int& b) { r = (c >> 11 & 31) * 255 / 31; g = (c >> 5 & 63) * 255 / 63; b = (c & 31) * 255 / 31; }
void Put32(std::string& s, std::uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back(static_cast<char>(v >> (8 * i))); }
void Chunk(std::string& out, const char* type, const std::string& data) {
    Put32(out, static_cast<std::uint32_t>(data.size()));
    const std::string body = std::string(type, 4) + data;
    out += body;
    Put32(out, static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size()))));
}
}

bool Entries(const std::string& file, std::vector<std::string>& dds) {
    std::string emb;
    if (!hud_layout::Unpack(file, emb) && file.compare(0, 4, "#EMB") != 0) return false;
    if (file.compare(0, 4, "#EMB") == 0) emb = file;
    if (emb.size() < 0x20 || emb.compare(0, 4, "#EMB") != 0) return false;
    const std::uint32_t count = U32(emb, 0xC), table = U32(emb, 0x18);
    if (count > 4096 || table + 8ull * count > emb.size()) return false;
    dds.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        const size_t slot = table + 8ull * i, start = slot + U32(emb, slot), size = U32(emb, slot + 4);
        if (start > emb.size() || size > emb.size() - start) return false;
        dds.push_back(emb.substr(start, size));
    }
    return true;
}

bool Decode(const std::string& dds, Image& image) {
    if (dds.size() < 128 || dds.compare(0, 4, "DDS ") != 0) return false;
    const std::uint32_t height = U32(dds, 12), width = U32(dds, 16);
    if (!width || !height || width > 4096 || height > 4096) return false;
    image.width = static_cast<int>(width); image.height = static_cast<int>(height);
    image.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    const auto* in = reinterpret_cast<const std::uint8_t*>(dds.data()) + 128;
    const size_t have = dds.size() - 128;
    if (dds.compare(84, 4, "DXT5") == 0) {
        const std::uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
        if (have < static_cast<size_t>(bx) * by * 16) return false;
        for (std::uint32_t y = 0; y < by; ++y) for (std::uint32_t x = 0; x < bx; ++x) {
            const std::uint8_t* b = in + (static_cast<size_t>(y) * bx + x) * 16;
            int alpha[8] = { b[0], b[1] };
            for (int i = 2; i < 8; ++i) alpha[i] = alpha[0] > alpha[1] ? ((8 - i) * alpha[0] + (i - 1) * alpha[1]) / 7 : i < 6 ? ((6 - i) * alpha[0] + (i - 1) * alpha[1]) / 5 : i == 6 ? 0 : 255;
            std::uint64_t alphaBits = 0;
            for (int i = 0; i < 6; ++i) alphaBits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
            const unsigned c0 = b[8] | b[9] << 8, c1 = b[10] | b[11] << 8;
            int col[4][3]; Colour565(c0, col[0][0], col[0][1], col[0][2]); Colour565(c1, col[1][0], col[1][1], col[1][2]);
            for (int k = 0; k < 3; ++k) { col[2][k] = (2 * col[0][k] + col[1][k]) / 3; col[3][k] = (col[0][k] + 2 * col[1][k]) / 3; }
            const std::uint32_t bits = b[12] | b[13] << 8 | b[14] << 16 | static_cast<std::uint32_t>(b[15]) << 24;
            for (int py = 0; py < 4; ++py) for (int px = 0; px < 4; ++px) {
                const std::uint32_t X = x * 4 + px, Y = y * 4 + py;
                if (X >= width || Y >= height) continue;
                const int i = py * 4 + px, c = bits >> (2 * i) & 3;
                std::uint8_t* out = &image.rgba[(static_cast<size_t>(Y) * width + X) * 4];
                out[0] = static_cast<std::uint8_t>(col[c][0]); out[1] = static_cast<std::uint8_t>(col[c][1]); out[2] = static_cast<std::uint8_t>(col[c][2]);
                out[3] = static_cast<std::uint8_t>(alpha[alphaBits >> (3 * i) & 7]);
            }
        }
        return true;
    }
    if (U32(dds, 88) != 32 || have < static_cast<size_t>(width) * height * 4) return false;
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {       // B G R A bytes
        image.rgba[i * 4] = in[i * 4 + 2]; image.rgba[i * 4 + 1] = in[i * 4 + 1]; image.rgba[i * 4 + 2] = in[i * 4]; image.rgba[i * 4 + 3] = in[i * 4 + 3];
    }
    return true;
}

std::string EncodePng(const Image& image) {
    std::string raw;
    const size_t row = static_cast<size_t>(image.width) * 4;
    raw.reserve((row + 1) * image.height);
    for (int y = 0; y < image.height; ++y) { raw.push_back(0); raw.append(reinterpret_cast<const char*>(&image.rgba[y * row]), row); }
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    std::string packed(size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(&packed[0]), &size, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()), 4) != Z_OK) return {};
    packed.resize(size);
    std::string head;
    Put32(head, static_cast<std::uint32_t>(image.width)); Put32(head, static_cast<std::uint32_t>(image.height));
    head += std::string("\x08\x06\x00\x00\x00", 5);                          // 8 bit RGBA
    std::string png("\x89PNG\r\n\x1a\n", 8);
    Chunk(png, "IHDR", head); Chunk(png, "IDAT", packed); Chunk(png, "IEND", "");
    return png;
}

namespace {
// Nearest of the palette for one colour: its index.
int Nearest(const int (*palette)[3], int count, const int* c) {
    int best = 0, most = 1 << 30;
    for (int i = 0; i < count; ++i) {
        const int d = (palette[i][0] - c[0]) * (palette[i][0] - c[0]) + (palette[i][1] - c[1]) * (palette[i][1] - c[1]) + (palette[i][2] - c[2]) * (palette[i][2] - c[2]);
        if (d < most) { most = d; best = i; }
    }
    return best;
}
unsigned To565(const int* c) { return (c[0] * 31 / 255) << 11 | (c[1] * 63 / 255) << 5 | (c[2] * 31 / 255); }
// A DXT5 block from the 4x4 texels of the image at (bx, by) (texels outside the image repeat the edge).
void EncodeBlock(const Image& im, int bx, int by, std::uint8_t* out) {
    int px[16][4], lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 }, a0 = 0, a1 = 255;
    for (int i = 0; i < 16; ++i) {
        const int X = std::min(bx * 4 + i % 4, im.width - 1), Y = std::min(by * 4 + i / 4, im.height - 1);
        for (int k = 0; k < 4; ++k) px[i][k] = im.rgba[(static_cast<size_t>(Y) * im.width + X) * 4 + k];
        a0 = std::max(a0, px[i][3]); a1 = std::min(a1, px[i][3]);
        if (px[i][3] > 0) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], px[i][k]); hi[k] = std::max(hi[k], px[i][k]); }
    }
    if (hi[0] < lo[0]) for (int k = 0; k < 3; ++k) lo[k] = hi[k] = 0;
    int alpha[8] = { a0, a1 };
    if (a0 == a1) { if (a0 == 255) { a1 = 254; alpha[1] = 254; } else { a0 = a1 + 1; alpha[0] = a0; } }
    for (int i = 2; i < 8; ++i) alpha[i] = ((8 - i) * alpha[0] + (i - 1) * alpha[1]) / 7;
    std::uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, most = 1 << 30;
        for (int j = 0; j < 8; ++j) { const int d = std::abs(alpha[j] - px[i][3]); if (d < most) { most = d; best = j; } }
        bits |= static_cast<std::uint64_t>(best) << (3 * i);
    }
    out[0] = static_cast<std::uint8_t>(alpha[0]); out[1] = static_cast<std::uint8_t>(alpha[1]);
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<std::uint8_t>(bits >> (8 * i));
    unsigned c0 = To565(hi), c1 = To565(lo);
    int p[4][3];
    Colour565(c0, p[0][0], p[0][1], p[0][2]); Colour565(c1, p[1][0], p[1][1], p[1][2]);
    for (int k = 0; k < 3; ++k) { p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3; }
    std::uint32_t idx = 0;
    for (int i = 0; i < 16; ++i) idx |= static_cast<std::uint32_t>(Nearest(p, 4, px[i])) << (2 * i);
    out[8] = c0 & 255; out[9] = c0 >> 8; out[10] = c1 & 255; out[11] = c1 >> 8;
    for (int i = 0; i < 4; ++i) out[12 + i] = static_cast<std::uint8_t>(idx >> (8 * i));
}
std::uint32_t Be32(const std::string& d, size_t at) {
    return at + 4 <= d.size() ? (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[at])) << 24 | static_cast<std::uint8_t>(d[at + 1]) << 16 | static_cast<std::uint8_t>(d[at + 2]) << 8 | static_cast<std::uint8_t>(d[at + 3])) : 0;
}
}

bool DecodePng(const std::string& png, Image& image) {
    if (png.size() < 33 || png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    std::string packed;
    int width = 0, height = 0;
    for (size_t at = 8; at + 12 <= png.size();) {
        const std::uint32_t size = Be32(png, at);
        const std::string type = png.substr(at + 4, 4);
        if (size > png.size() || at + 12 + size > png.size()) return false;
        if (type == "IHDR") {
            if (size < 13) return false;
            width = static_cast<int>(Be32(png, at + 8)); height = static_cast<int>(Be32(png, at + 12));
            if (png[at + 16] != 8 || png[at + 17] != 6 || png[at + 20] != 0) return false;       // 8 bit RGBA, not interlaced
        } else if (type == "IDAT") packed.append(png, at + 8, size);
        at += 12ull + size;
    }
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    const size_t row = static_cast<size_t>(width) * 4;
    std::vector<unsigned char> raw;
    if (!zinflate::InflateBounded(reinterpret_cast<const unsigned char*>(packed.data()), packed.size(), zinflate::Format::Zlib, (row + 1) * height, raw, (row + 1) * height)) return false;
    image.width = width; image.height = height; image.rgba.assign(row * height, 0);
    for (int y = 0; y < height; ++y) {
        const int filter = static_cast<std::uint8_t>(raw[y * (row + 1)]);
        const auto* in = reinterpret_cast<const std::uint8_t*>(&raw[y * (row + 1) + 1]);
        std::uint8_t* out = &image.rgba[y * row];
        const std::uint8_t* up = y ? &image.rgba[(y - 1) * row] : nullptr;
        for (size_t i = 0; i < row; ++i) {
            const int a = i >= 4 ? out[i - 4] : 0, b = up ? up[i] : 0, c = up && i >= 4 ? up[i - 4] : 0;
            int predict = 0;
            if (filter == 1) predict = a; else if (filter == 2) predict = b; else if (filter == 3) predict = (a + b) / 2;
            else if (filter == 4) { const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c); predict = pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
            else if (filter != 0) return false;
            out[i] = static_cast<std::uint8_t>(in[i] + predict);
        }
    }
    return true;
}

bool Paste(const std::string& texEmz, int entry, const Image& picture, int x, int y, std::string& out) {
    std::string emb;
    if (!hud_layout::Unpack(texEmz, emb) || emb.compare(0, 4, "#EMB") != 0) return false;
    const std::uint32_t count = U32(emb, 0xC), table = U32(emb, 0x18);
    if (entry < 0 || static_cast<std::uint32_t>(entry) >= count || table + 8ull * count > emb.size()) return false;
    const size_t slot = table + 8ull * entry, start = slot + U32(emb, slot), size = U32(emb, slot + 4);
    if (start > emb.size() || size > emb.size() - start) return false;
    std::string dds = emb.substr(start, size);
    Image sheet;
    if (!Decode(dds, sheet) || x < 0 || y < 0 || x + picture.width > sheet.width || y + picture.height > sheet.height) return false;
    for (int j = 0; j < picture.height; ++j)
        std::memcpy(&sheet.rgba[(static_cast<size_t>(y + j) * sheet.width + x) * 4], &picture.rgba[static_cast<size_t>(j) * picture.width * 4], static_cast<size_t>(picture.width) * 4);
    if (dds.compare(84, 4, "DXT5") == 0) {
        const int bx = (sheet.width + 3) / 4;
        for (int by = y / 4; by <= (y + picture.height - 1) / 4; ++by) for (int bxi = x / 4; bxi <= (x + picture.width - 1) / 4; ++bxi)
            EncodeBlock(sheet, bxi, by, reinterpret_cast<std::uint8_t*>(&dds[128 + (static_cast<size_t>(by) * bx + bxi) * 16]));
    } else {
        for (int j = 0; j < picture.height; ++j) for (int i = 0; i < picture.width; ++i) {
            const std::uint8_t* from = &picture.rgba[(static_cast<size_t>(j) * picture.width + i) * 4];
            std::uint8_t* to = reinterpret_cast<std::uint8_t*>(&dds[128 + (static_cast<size_t>(y + j) * sheet.width + x + i) * 4]);
            to[0] = from[2]; to[1] = from[1]; to[2] = from[0]; to[3] = from[3];
        }
    }
    emb.replace(start, size, dds);
    out = hud_layout::Pack(emb);
    return true;
}
} } }
