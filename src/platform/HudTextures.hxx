#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The texture sheets of a HUD (battle\hud\...\*.tex.emz): an #EMZ holding an #EMB of
// DDS images (DXT5 or 32 bit BGRA, no mips), decoded to RGBA and written as PNG for the editor's page.
namespace sf4e { namespace platform { namespace hud_textures {
struct Image { int width = 0, height = 0; std::vector<std::uint8_t> rgba; };
// The DDS files in a .tex.emz (or a plain #EMB), in order. False when it cannot be read.
bool Entries(const std::string& file, std::vector<std::string>& dds);
// DXT5 and 32 bit BGRA only. False for anything else.
bool Decode(const std::string& dds, Image& image);
std::string EncodePng(const Image& image);
// 8 bit RGBA PNG, not interlaced (what EncodePng writes, and what a browser canvas exports). False for anything else.
bool DecodePng(const std::string& png, Image& image);
// Copies the picture into a sheet's entry at (x, y) and returns the .tex.emz with it, in the entry's own format (a DXT5
// entry has only the blocks the picture touches encoded again). False when it does not fit or the entry cannot be written.
bool Paste(const std::string& texEmz, int entry, const Image& picture, int x, int y, std::string& out);
} } }
