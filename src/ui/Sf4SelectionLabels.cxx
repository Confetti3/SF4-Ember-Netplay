// SF4's pick labels (SelectionLabels.hxx): its costumes and costume packs,
// its Ultras, and its stages.
#include "SelectionLabels.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/InstalledContent.hxx"
#include "../common/Localization.hxx"
#include "../common/StageCatalog.hxx"

namespace sf4e { namespace ui {
std::string StageLabel(int stageId, const selection::InstalledContent* installed) {
    if (selection::IsRandomStage(stageId)) return loc::T("selection.random_stage");
    const int id = selection::NormalizeStage(stageId);
    if (const auto* stage = selection::FindStage(id)) return stage->name;
    // A custom stage is named by the setup program, or by its code.
    const std::string name = installed ? installed->StageName(id) : std::string();
    return name.empty() ? loc::Tf("selection.custom", selection::StageCode(id)) : name;
}
std::string CostumeLabel(const selection::Pick& pick, const selection::InstalledContent* installed) {
    if (pick.costume == 0) return loc::T("selection.original");
    // A custom costume is named by the setup program, or by its slot as it numbers them (slot = index + 1).
    if (selection::IsCustomCostume(pick.costume)) {
        const std::string name = installed ? installed->Name(pick.fighter, pick.costume) : std::string();
        return name.empty() ? loc::Tf("selection.custom", pick.costume + 1) : name;
    }
    return loc::Tf("selection.alternate_pack", pick.costume, selection::CostumePack(pick.fighter, pick.costume));
}
const char* UltraLabel(int ultra) {
    return loc::T(ultra==2?"selection.ultra_double":ultra==1?"selection.ultra_two":"selection.ultra_one");
}
std::string ShortCostumeLabel(const selection::Pick& pick, const selection::InstalledContent* installed) {
    const std::string pack = selection::CostumePack(pick.fighter, pick.costume);
    return pick.costume > 0 && pack == "Alternate" ? loc::Tf("home.selection_alternate", pick.costume) :
        pick.costume > 0 && !pack.empty() ? pack : CostumeLabel(pick, installed);
}
std::string ShortAppearanceLabel(const selection::Pick& pick, const selection::InstalledContent* installed) {
    return loc::Tf("home.selection_appearance", ShortCostumeLabel(pick, installed), pick.color + 1);
}
const char* ShortUltraLabel(int ultra) {
    return loc::T(ultra==2?"home.selection_ultra_double":ultra==1?"home.selection_ultra_two":"home.selection_ultra_one");
}
// The old Ultra page's badge, the same in every language.
const char* UltraBadge(int ultra) {
    return ultra==2?"W":ultra==1?"II":ultra==0?"I":"";
}
std::string UltraMove(int fighter, int ultra) {
    const auto* found = selection::FindFighter(fighter);
    if (!found) return {};
    return ultra == 2 ? std::string(UltraLabel(ultra)) : std::string(found->ultras[ultra == 1 ? 1 : 0]);
}
std::string UltraName(int fighter, int ultra) {
    const std::string move = UltraMove(fighter, ultra);
    return move.empty() || ultra == 2 ? move : std::string(UltraLabel(ultra)) + ": " + move;
}
} }
