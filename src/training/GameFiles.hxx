#pragma once
#include "../common/BoundedRead.hxx"
#include "../platform/Utf8Path.hxx"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

// Finds and reads a fighter's own files in the game folder. It knows nothing
// of the running game or the overlay.
namespace sf4e { namespace training {
// Reads a whole file of at most `most` bytes.
inline bool ReadFile(const std::filesystem::path& path, std::size_t most, std::vector<std::uint8_t>& bytes, std::string& error) {
    const std::string name = platform::PathToUtf8(path.filename());
    auto read = durable::ReadBounded(path, most);
    switch (read.status) {
    case durable::ReadStatus::Read: bytes = std::move(read.bytes); return true;
    case durable::ReadStatus::TooLarge: error = name + " is too large"; return false;
    case durable::ReadStatus::CannotRead: error = "cannot read " + name; return false;
    default: error = "cannot open " + name; return false;
    }
}
// The regulation folder the game reads a fighter's .bac and .bcm from, by
// the edition the player picked and the fighter's id, as
// CommonResourceHolder::load (0x5E88E0) chooses it; "" for battle\chara
// itself, nullptr for an edition it has no folder for.
inline const char* RegulationFolder(int edition, int id) {
    const auto in = [id](std::initializer_list<int> ids) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    switch (edition) {
    case 0: return "";
    case 1: return in({0, 1, 9, 10, 11, 15, 17, 21, 27, 29}) ? "v101" : "";
    case 2: return "latest";
    case 3: return "latest_ae";
    case 4: return in({0, 10, 22, 28, 31, 34}) ? "v104" : "latest_ae";
    case 5: return "ae2";
    case 6: return "concept";
    case 7: return "sf4";
    case 8: return id >= 0 && id < 0x2C && id != 10 && id != 13 ? "ae2_109" : "ae2";
    case 9: case 11: return in({1, 3, 5, 9, 12, 14, 15, 16, 17, 18, 20, 21, 23, 24, 25, 26}) ? "sf4_109" : "sf4";
    case 10:
        if (in({0, 1, 4, 5, 6, 8, 11, 12, 14, 16, 17, 18, 19, 20, 22, 23, 25, 26, 29, 30, 32, 33, 34, 36, 37, 38, 40, 42})) return "ae2_109";
        return in({2, 3, 7, 9, 10, 15, 21, 24, 27, 28, 31, 35, 39, 41, 43}) ? "ae2_109b" : "ae2";
    case 12: return id >= 0 && id < 0x2C ? "ae2_110" : "ae2";
    case 13:
        if (in({0, 4, 9, 16, 17, 20, 21, 23, 25, 26})) return "sf4_110";
        return in({1, 3, 5, 12, 14, 15, 18, 24}) ? "sf4_109" : "sf4";
    case 14: return in({0, 6, 9, 11, 15, 20, 26, 27, 31, 39, 41, 43}) ? "ae2_111" : "ae2_110";
    case 16: return "concept_111";
    default: return nullptr;
    }
}
// The folder a fighter's .bcm and .bac are read from. The install is layered
// (SF4, SSF4, AE and USF4 as resource, dlc and patch folders, newest first)
// and the game reads a folder from the newest layer that has it, so the
// edition's folder (RegulationFolder) is looked for in each layer in turn.
// An edition without one, or an install that lacks it, falls back on USF4's.
inline std::filesystem::path CommandFolder(const std::filesystem::path& game, const std::string& code, int id, int edition) {
    static const char* const layers[] = {"patch_ae2_tu3", "patch_ae2_tu2", "patch_ae2_tu1b", "patch_ae2_tu1", "patch_ae2",
        "dlc/04_ae2", "dlc/03_character_free", "patch", "resource"};
    std::error_code ignored;
    for (const int one : {edition, 14}) {
        const char* folder = RegulationFolder(one, id);
        if (!folder) continue;
        for (const char* layer : layers) {
            const auto found = (*folder ? game / layer / "battle/regulation" / folder : game / layer / "battle/chara") / code;
            if (std::filesystem::exists(found / (code + ".bcm"), ignored)) return found;
        }
    }
    return game / "resource/battle/chara" / code;
}
} }
