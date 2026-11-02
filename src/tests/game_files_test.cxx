#include "../training/GameFiles.hxx"
#include "test_support.hxx"
#include <cstring>
#include <fstream>

using namespace sf4e::training;

// The game's own folder choice (CommonResourceHolder::load), and that a
// fighter's files are found in the newest layer that has that folder.
int main() {
    const auto is = [](const char* folder, const char* want) { return folder && std::strcmp(folder, want) == 0; };
    CHECK(is(RegulationFolder(16, 1), "concept_111") && is(RegulationFolder(6, 1), "concept"));
    CHECK(is(RegulationFolder(14, 0), "ae2_111") && is(RegulationFolder(14, 1), "ae2_110") && is(RegulationFolder(14, 43), "ae2_111"));
    CHECK(is(RegulationFolder(13, 0), "sf4_110") && is(RegulationFolder(13, 1), "sf4_109") && is(RegulationFolder(13, 2), "sf4"));
    CHECK(is(RegulationFolder(0, 5), "") && RegulationFolder(15, 1) == nullptr && RegulationFolder(99, 1) == nullptr);

    const auto game = std::filesystem::temp_directory_path() / "sf4e-game-files-test";
    std::error_code ignored;
    std::filesystem::remove_all(game, ignored);
    const auto file = [&](const char* folder) {
        const auto at = game / folder / "KEN";
        std::filesystem::create_directories(at);
        std::ofstream(at / "KEN.bcm") << "x";
        return at;
    };
    const auto omega = file("patch_ae2_tu3/battle/regulation/concept_111");
    const auto ultra = file("patch_ae2_tu2/battle/regulation/ae2_110");
    file("dlc/04_ae2/battle/regulation/ae2_110");
    CHECK(CommandFolder(game, "KEN", 1, 16) == omega);
    CHECK(CommandFolder(game, "KEN", 1, 14) == ultra);
    // An edition with no folder, or one the install lacks, reads USF4's.
    CHECK(CommandFolder(game, "KEN", 1, 99) == ultra && CommandFolder(game, "KEN", 1, 6) == ultra);
    std::filesystem::remove_all(game, ignored);
    return 0;
}
