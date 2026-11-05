// The fake game's pick labels (SelectionLabels.hxx): plain names by number,
// with nothing of SF4's costume packs, Ultras or stages.
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
} }
