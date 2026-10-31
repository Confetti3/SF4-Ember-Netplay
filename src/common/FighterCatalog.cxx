#include "FighterCatalog.hxx"
#include "InstalledContent.hxx"
#include <algorithm>

namespace sf4e { namespace selection {
namespace {
const Fighter fighters[] = {
#include "FighterMetadata.inc"
};
static_assert(sizeof(fighters) / sizeof(fighters[0]) == FighterCount, "Every native fighter needs metadata");
const std::array<Edition, 6> editions = {{
    {13, "Street Fighter IV", "SFIV"},
    {1, "Super Street Fighter IV", "SSFIV"},
    {2, "Arcade Edition", "AE"},
    {4, "Arcade Edition Ver. 2012", "AE 2012"},
    {14, "Ultra Street Fighter IV", "USFIV"},
    {16, "Omega", "OMEGA"}
}};
}

const Fighter* FindFighter(int nativeId) {
    return nativeId >= 0 && nativeId < FighterCount ? &fighters[nativeId] : nullptr;
}
const std::array<Edition, 6>& EditionList() { return editions; }
const Edition* FindEdition(int nativeId) {
    for (const auto& edition : editions) if (edition.id == nativeId) return &edition;
    return nullptr;
}
bool EditionAllowed(int fighterId, int editionId, bool editionSelect) {
    const auto* fighter = FindFighter(fighterId);
    return fighter && FindEdition(editionId) && (editionSelect || editionId == UltraEdition) &&
           (fighter->editions & (std::uint32_t(1) << editionId)) != 0;
}
std::vector<int> AllowedEditions(int fighterId, bool editionSelect) {
    std::vector<int> result;
    for (const auto& edition : editions)
        if (EditionAllowed(fighterId, edition.id, editionSelect)) result.push_back(edition.id);
    return result;
}
int NormalizeEdition(int fighterId, int editionId, bool editionSelect) {
    if (EditionAllowed(fighterId, editionId, editionSelect)) return editionId;
    return EditionAllowed(fighterId, UltraEdition, editionSelect) ? UltraEdition : -1;
}

int BaseCostumeCount(int fighterId) {
    const auto* fighter = FindFighter(fighterId);
    // Matches the native table at RVA 0x566158, used by 0x2a07b0.
    return !fighter ? 0 : (fighter->editions & (1u << 13)) ? 4 : 3;
}
int CostumeCount(int fighterId) {
    const int base = BaseCostumeCount(fighterId);
    // The released Vacation, Wild, and Horror packs follow the earlier alts.
    // Native storage reserves eight slots; the unused future slot is not an outfit.
    return base ? base + 3 : 0;
}
int ColorCount(int fighterId, int costumeId) {
    if (FindFighter(fighterId) && IsCustomCostume(costumeId)) return CustomColorCount;
    if (costumeId < 0 || costumeId >= CostumeCount(fighterId)) return 0;
    // Native customization builds 12 earlier-costume and 22 later-costume
    // entries at RVA 0x2ec64. Availability further filters profile unlocks.
    return costumeId < BaseCostumeCount(fighterId) ? 12 : 22;
}
const char* CostumePack(int fighterId, int costumeId) {
    if (costumeId < 0 || costumeId >= CostumeCount(fighterId)) return "";
    if (costumeId == 0) return "Original";
    const int base = BaseCostumeCount(fighterId);
    if (costumeId < base) return "Alternate";
    const char* packs[] = {"Vacation", "Wild", "Horror"};
    return packs[costumeId - base];
}
std::vector<int> AllowedUltras(int fighterId, int editionId) {
    if (!EditionAllowed(fighterId, editionId, true)) return {};
    if (editionId == 13) return {0};
    if (editionId == UltraEdition) return {0, 1, 2};
    return {0, 1};
}
bool FighterLocked(int fighterId, const Availability& availability) {
    // 6A0970 licence-gates only 35-43 (39-43 in some editions). The base
    // roster's bit 16 comes from a default table, so it never locks them.
    return fighterId >= 35 && availability.ready && !(availability.costumes & 1u);
}
std::vector<int> AllowedCostumes(int fighterId, const Availability& availability, const InstalledContent* installed) {
    std::vector<int> result;
    // A fighter's other costume bits can be set without the fighter itself
    // (the title grants costume 1 to 39-43), so an unowned fighter has none.
    if (availability.ready && !FighterLocked(fighterId, availability))
        for (int costume = 0; costume < CostumeCount(fighterId); ++costume)
            if (availability.costumes & (1u << costume)) result.push_back(costume);
    // Custom costumes need no licence of their own, only the fighter's.
    if (installed && availability.ready && !FighterLocked(fighterId, availability) && FindFighter(fighterId)) {
        const auto& customs = installed->Costumes(fighterId);
        result.insert(result.end(), customs.begin(), customs.end());
    }
    return result;
}
std::vector<int> AllowedColors(int fighterId, int costumeId, const Availability& availability, const InstalledContent* installed) {
    std::vector<int> result;
    const auto costumes = AllowedCostumes(fighterId, availability, installed);
    if (std::find(costumes.begin(), costumes.end(), costumeId) == costumes.end()) return result;
    if (IsCustomCostume(costumeId))
        for (int color = 0; color < CustomColorCount; ++color) result.push_back(color);
    else
        for (int color = 0; color < ColorCount(fighterId, costumeId); ++color)
            if (availability.colors[costumeId] & (1u << color)) result.push_back(color);
    if (installed && availability.ready) {
        const auto& customs = installed->Colors(fighterId, costumeId);
        result.insert(result.end(), customs.begin(), customs.end());
    }
    return result;
}
// The costume's own colors, or a custom color, of a costume the fighter has.
bool ColorInRange(int fighterId, int costumeId, int color) {
    const int count = ColorCount(fighterId, costumeId);
    return count > 0 && color >= 0 && (color < count || IsCustomColor(color));
}
bool Valid(const Pick& pick, bool editionSelect) {
    // The native menus wrap PA over -1..9 and quotes over -1..10.
    // Their signed -1 sentinel is transported as byte 255 (None / Random).
    if ((pick.personalAction != 255 && (pick.personalAction < 0 || pick.personalAction > 9)) ||
        (pick.winQuote != 255 && (pick.winQuote < 0 || pick.winQuote > 10)) ||
        pick.handicap < 0 || pick.handicap > 4) return false;
    if (!EditionAllowed(pick.fighter, pick.edition, editionSelect) || pick.costume < 0 ||
        (pick.costume >= CostumeCount(pick.fighter) && !IsCustomCostume(pick.costume)) ||
        !ColorInRange(pick.fighter, pick.costume, pick.color)) return false;
    const auto ultras = AllowedUltras(pick.fighter, pick.edition);
    return std::find(ultras.begin(), ultras.end(), pick.ultra) != ultras.end();
}
bool Available(const Pick& pick, bool editionSelect, const Availability& availability, const InstalledContent* installed) {
    if (!Valid(pick, editionSelect)) return false;
    const auto actions = AllowedPersonalActions(availability);
    if (std::find(actions.begin(), actions.end(), pick.personalAction) == actions.end()) return false;
    const auto colors = AllowedColors(pick.fighter, pick.costume, availability, installed);
    return std::find(colors.begin(), colors.end(), pick.color) != colors.end();
}
bool Normalize(Pick& pick, bool editionSelect, const Availability* availability, const InstalledContent* installed) {
    const Pick before = pick;
    if (pick.personalAction != 255 && (pick.personalAction < 0 || pick.personalAction > 9)) pick.personalAction = 255;
    if (pick.winQuote != 255 && (pick.winQuote < 0 || pick.winQuote > 10)) pick.winQuote = 255;
    if (pick.handicap < 0 || pick.handicap > 4) pick.handicap = 0;
    if (!FindFighter(pick.fighter)) pick.fighter = 0;
    pick.edition = NormalizeEdition(pick.fighter, pick.edition, editionSelect);
    if (pick.costume < 0 || (pick.costume >= CostumeCount(pick.fighter) && !IsCustomCostume(pick.costume))) pick.costume = 0;
    if (!ColorInRange(pick.fighter, pick.costume, pick.color)) pick.color = 0;
    const auto ultras = AllowedUltras(pick.fighter, pick.edition);
    if (std::find(ultras.begin(), ultras.end(), pick.ultra) == ultras.end()) pick.ultra = 0;
    // Unknown availability is not a reason to destroy a saved valid pick.
    if (availability && FighterLocked(pick.fighter, *availability)) {
        // Ryu is in every edition the game sells. His own availability
        // settles the costume and color on the next read.
        pick.fighter = 0; pick.costume = 0; pick.color = 0; pick.ultra = 0;
        pick.edition = NormalizeEdition(0, pick.edition, editionSelect);
    } else if (availability && availability->ready) {
        const auto actions = AllowedPersonalActions(*availability);
        if (std::find(actions.begin(), actions.end(), pick.personalAction) == actions.end()) pick.personalAction = 255;
        const auto costumes = AllowedCostumes(pick.fighter, *availability, installed);
        if (!costumes.empty() && std::find(costumes.begin(), costumes.end(), pick.costume) == costumes.end())
            pick.costume = costumes.front();
        const auto colors = AllowedColors(pick.fighter, pick.costume, *availability, installed);
        if (!colors.empty() && std::find(colors.begin(), colors.end(), pick.color) == colors.end()) pick.color = colors.front();
    }
    return before.fighter != pick.fighter || before.costume != pick.costume || before.color != pick.color ||
           before.ultra != pick.ultra || before.edition != pick.edition || before.personalAction != pick.personalAction ||
           before.winQuote != pick.winQuote || before.handicap != pick.handicap;
}

std::vector<int> AllowedPersonalActions(const Availability& availability) {
    if (!availability.ready) return {};
    std::vector<int> actions{255};
    for (int action = 0; action < 10; ++action)
        if (availability.personalActions & (1u << action)) actions.push_back(action);
    return actions;
}

} }
