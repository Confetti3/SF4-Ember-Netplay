#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Finds and reads a fighter's own files in the game folder. It knows nothing
// of the running game or the overlay.
namespace sf4e { namespace training {
// Reads a whole file of at most `most` bytes.
inline bool ReadFile(const std::filesystem::path& path, std::size_t most, std::vector<std::uint8_t>& bytes, std::string& error) {
    const auto name = path.filename().u8string();
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = in ? static_cast<std::streamoff>(in.tellg()) : -1;
    if (size < 0) { error = "cannot open " + std::string(name.begin(), name.end()); return false; }
    if (static_cast<std::uint64_t>(size) > most) { error = std::string(name.begin(), name.end()) + " is too large"; return false; }
    std::vector<std::uint8_t> read(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size && !in.read(reinterpret_cast<char*>(read.data()), size)) { error = "cannot read " + std::string(name.begin(), name.end()); return false; }
    bytes = std::move(read);
    return true;
}
// The folder a fighter's .bcm and .bac are read from. The install is layered
// (SF4, SSF4, AE and USF4 as resource, dlc and patch folders) and a fighter's
// files sit in several layers. From the program: USF4 (edition 14) switches
// per fighter, the 1.11 balance for `later` and 1.10 for the rest, and that
// choice is tried first. The rest is a fallback for an install that lacks it:
// the other layers newest first, down to the SF4 resource folder. Whether the
// game loads the file found is not verified.
inline std::filesystem::path CommandFolder(const std::filesystem::path& game, const std::string& code) {
    static const char* const later[] = {"RYU", "GUL", "BLR", "VEG", "JHA", "CMY", "DAN", "GUY", "DDL", "RLN", "PSN", "DCP"};
    static const char* const layers[] = {"patch_ae2_tu3/battle/regulation/ae2_111", "patch_ae2_tu2/battle/regulation/ae2_110",
        "patch_ae2_tu1/battle/regulation/ae2_109", "dlc/04_ae2/battle/regulation/ae2", "dlc/03_character_free/battle/regulation/latest",
        "patch/battle/regulation/latest_ae", "resource/battle/chara"};
    const bool newest = std::any_of(std::begin(later), std::end(later), [&](const char* one) { return code == one; });
    std::filesystem::path found;
    std::error_code ignored;
    for (std::size_t i = newest ? 0 : 1; i < sizeof layers / sizeof *layers; ++i) {
        found = game / layers[i] / code;
        if (std::filesystem::exists(found / (code + ".bcm"), ignored)) break;
    }
    return found;
}
} }
