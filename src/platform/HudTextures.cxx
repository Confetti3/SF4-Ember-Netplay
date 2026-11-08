#include "HudTextures.hxx"
#include "HudLayout.hxx"
#include <zlib.h>
#include <algorithm>
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
} } }
