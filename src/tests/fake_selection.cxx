// The fake game's pick labels (SelectionLabels.hxx), pictures
// (SelectionArt.hxx) and packaged selection files (SelectionAssetPath.hxx):
// plain names by number, no picture of its own and no files, with nothing of
// SF4's costume packs, Ultras, stages or files.
#include "../common/SelectionAssetPath.hxx"
#include "../ui/SelectionArt.hxx"
#include "../ui/SelectionLabels.hxx"

#include <string>

namespace sf4e { namespace selection {
bool IsSelectionAssetPath(const std::wstring&) { return false; }
} }

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
std::string UltraMove(int, int) { return "Ultra"; }
std::string UltraName(int, int) { return "Ultra"; }

namespace {
// Missing, so a screen says so rather than waiting for a picture.
SelectionImage None() { SelectionImage image; image.missing = true; return image; }
}
SelectionImage SelectionArt::Portrait(int, bool) { return None(); }
SelectionImage SelectionArt::Character(int, int, int, bool) { return None(); }
SelectionImage SelectionArt::Appearance(int, int, int) { return None(); }
SelectionImage SelectionArt::Ultra(int, int) { return None(); }
SelectionImage SelectionArt::UltraIcon(int) { return None(); }
SelectionImage SelectionArt::Stage(int) { return None(); }
SelectionImage SelectionArt::RankingEmblem(const std::string&) { return None(); }
} }
