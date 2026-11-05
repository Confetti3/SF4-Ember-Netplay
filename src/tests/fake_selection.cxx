// The fake game's pick labels (SelectionLabels.hxx) and pictures
// (SelectionArt.hxx): plain names by number, and no picture of its own, with
// nothing of SF4's costume packs, Ultras, stages or files.
#include "../ui/SelectionArt.hxx"
#include "../ui/SelectionLabels.hxx"

#include <string>

namespace sf4e { namespace ui {
std::string StageLabel(int stageId, const selection::InstalledContent*) { return "Stage " + std::to_string(stageId); }
std::string CostumeLabel(const selection::Pick& pick, const selection::InstalledContent*) {
    return "Costume " + std::to_string(pick.costume);
}
std::string ShortCostumeLabel(const selection::Pick& pick, const selection::InstalledContent* installed) {
    return CostumeLabel(pick, installed);
}
std::string ShortAppearanceLabel(const selection::Pick& pick, const selection::InstalledContent* installed) {
    return CostumeLabel(pick, installed) + ", color " + std::to_string(pick.color + 1);
}
const char* UltraLabel(int) { return "Ultra"; }
const char* ShortUltraLabel(int) { return "Ultra"; }

namespace {
// Missing, so a screen says so rather than waiting for a picture.
SelectionImage None() { SelectionImage image; image.missing = true; return image; }
}
SelectionImage SelectionArt::Portrait(int, bool) { return None(); }
SelectionImage SelectionArt::Character(int, int, int, bool) { return None(); }
SelectionImage SelectionArt::Appearance(int, int, int) { return None(); }
SelectionImage SelectionArt::Ultra(int, int) { return None(); }
SelectionImage SelectionArt::Stage(int) { return None(); }
SelectionImage SelectionArt::RankingEmblem(const std::string&) { return None(); }
} }
