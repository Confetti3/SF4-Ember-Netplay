#include "FighterCatalog.hxx"
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
std::vector<UltraCommand> UltraCommands(int fighterId, int ultraId, int editionId) {
    if (!EditionAllowed(fighterId, editionId, true) || ultraId < 0 || ultraId > 1 ||
        (editionId == 13 && ultraId != 0)) return {};
    // Native fighter order. Sources and edition exceptions: docs/design/ULTRA_INPUTS.md.
    static const char* commands[][2] = {
        {"236 236 + PPP", "236 236 + KKK"}, // Ryu
        {"236 236 + PPP", "236 236 + KKK"}, // Ken
        {"~4 6 4 6 + KKK", "236 236 + PPP"}, // Chun-Li
        {"~4 6 4 6 + PPP", "360 360 + PPP"}, // Honda
        {"~4 6 4 6 + PPP", "~1 3 1 9 + PPP"}, // Blanka
        {"360 360 + PPP", "360 360 + KKK"}, // Zangief
        {"~1 3 1 9 + KKK", "~4 6 4 6 + PPP"}, // Guile
        {"236 236 + PPP", "236 236 + KKK"}, // Dhalsim
        {"~4 6 4 6 + PPP", "360 360 + PPP"}, // Balrog (boxer)
        {"~1 3 1 9 + KKK", "~1 6 4 6 + KKK"}, // Vega (claw)
        {"236 236 + KKK", "236 236 + PPP"}, // Sagat
        {"~4 6 4 6 + KKK", "~4 6 4 6 + PPP"}, // M. Bison
        {"236 236 + PPP", "214 214 + KKK"}, // Viper
        {"236 236 + PPP", "214 214 + PPP"}, // Rufus
        {"236 236 + KKK", "214 214 + KKK"}, // El Fuerte
        {"236 236 + PPP", "236 236 + KKK"}, // Abel
        {"236 236 + PPP", "214 214 + PPP"}, // Seth
        {"LP LP 4 LK HP", "8 8 + KKK"}, // Akuma
        {"236 236 + PPP", "236 236 + KKK"}, // Gouken
        {"360 360 + PPP", "63214 63214 + KKK"}, // T. Hawk
        {"236 236 + KKK", "214 214 + PPP"}, // Cammy
        {"236 236 + PPP", "236 236 + KKK"}, // Fei Long
        {"~4 6 4 6 + KKK", "~1 3 1 9 + PPP"}, // Dee Jay
        {"214 214 + KKK", "236 236 + PPP"}, // Sakura
        {"236 236 + PPP", "214 214 + KKK"}, // Rose
        {"236 236 + PPP", "214 214 + PPP"}, // Gen
        {"236 236 + PPP", "236 236 + KKK"}, // Dan
        {"236 236 + KKK", "63214 63214 + PPP"}, // Guy
        {"236 236 + PPP", "214 214 + PPP"}, // Cody
        {"63214 63214 + PPP", "236 236 + KKK"}, // Ibuki
        {"236 236 + PPP", "236 236 + KKK"}, // Makoto
        {"236 236 + KKK", "236 236 + PPP"}, // Dudley
        {"214 214 + KKK", "236 236 + KKK"}, // Adon
        {"360 360 + PPP", "2 2 2 + KKK"}, // Hakan
        {"236 236 + PPP", "236 236 + KKK"}, // Juri
        {"236 236 + PPP", "236 236 + KKK"}, // Yun
        {"236 236 + PPP", "236 236 + KKK"}, // Yang
        {"236 236 + PPP", "236 236 + KKK"}, // Evil Ryu
        {"236 236 + PPP", "63214 63214 + PPP"}, // Oni
        {"236 236 + PPP", "236 236 + KKK"}, // Rolento
        {"236 236 + KKK", "236 236 + PPP"}, // Elena
        {"236 236 + PPP", "63214 63214 + PPP"}, // Poison
        {"360 360 + PPP", "236 236 + KKK"}, // Hugo
        {"~4 6 4 6 + PPP", "~4 6 4 6 + KKK"}, // Decapre
    };
    static_assert(sizeof(commands) / sizeof(commands[0]) == FighterCount, "Every fighter needs Ultra inputs");
    if (fighterId == 11 && ultraId == 1 && editionId == 1) return {{"236 236 + PPP", ""}};
    if (fighterId == 43 && editionId == 16) {
        if (ultraId == 0) return {{"236 236 + PPP", ""}};
        return {{"236 236 + KKK", "Ground"}, {"214 214 + KKK", "Anti-air"}};
    }
    if (fighterId == 43 && ultraId == 1)
        return {{commands[fighterId][ultraId], "Ground"}, {"~1 3 1 9 + KKK", "Anti-air"}};
    if (fighterId == 25)
        return {{commands[fighterId][ultraId], "Mantis stance"}, {"236 236 + KKK", ultraId == 0 ? "Crane stance" : "Crane stance / in air"}};
    if (fighterId == 38 && ultraId == 0)
        return {{"236 236 + PPP", "Ground or in air"}, {"236 236 + KKK", "Anti-air"}};
    if (fighterId == 4 && ultraId == 1)
        return {{commands[fighterId][ultraId], "Anti-air"}, {"~1 3 1 9 + KKK", "Ground"}};
    if (fighterId == 8 && ultraId == 0)
        return {{commands[fighterId][ultraId], "Punch start"}, {"~4 6 4 6 + KKK", "Kick start"}};
    if (fighterId == 23 && ultraId == 1)
        return {{commands[fighterId][ultraId], "Forward"}, {"236 236 + KKK", "Anti-air"}};
    const char* condition = "";
    if ((fighterId == 5 || fighterId == 7 || fighterId == 12) && ultraId == 1) condition = "In air";
    if ((fighterId == 3 || fighterId == 8 || fighterId == 27 || fighterId == 41) && ultraId == 1 ||
        (fighterId == 5 || fighterId == 19 || fighterId == 29 || fighterId == 33 || fighterId == 42) && ultraId == 0)
        condition = "Close range";
    if ((fighterId == 20 || fighterId == 21) && ultraId == 1) condition = "Counter";
    if (fighterId == 15 && ultraId == 1) condition = "Hold kicks to delay; punch to cancel";
    if (fighterId == 40 && ultraId == 1) condition = "Press PPP again to stop healing";
    return {{commands[fighterId][ultraId], condition}};
}
std::vector<int> AllowedCostumes(int fighterId, const Availability& availability) {
    std::vector<int> result;
    if (availability.ready)
        for (int costume = 0; costume < CostumeCount(fighterId); ++costume)
            if (availability.costumes & (1u << costume)) result.push_back(costume);
    return result;
}
std::vector<int> AllowedColors(int fighterId, int costumeId, const Availability& availability) {
    std::vector<int> result;
    const auto costumes = AllowedCostumes(fighterId, availability);
    if (std::find(costumes.begin(), costumes.end(), costumeId) == costumes.end()) return result;
    for (int color = 0; color < ColorCount(fighterId, costumeId); ++color)
        if (availability.colors[costumeId] & (1u << color)) result.push_back(color);
    return result;
}
bool Valid(const Pick& pick, bool editionSelect) {
    // The native menus wrap PA over -1..9 and quotes over -1..10.
    // Their signed -1 sentinel is transported as byte 255 (None / Random).
    if ((pick.personalAction != 255 && (pick.personalAction < 0 || pick.personalAction > 9)) ||
        (pick.winQuote != 255 && (pick.winQuote < 0 || pick.winQuote > 10)) ||
        pick.handicap < 0 || pick.handicap > 4) return false;
    if (!EditionAllowed(pick.fighter, pick.edition, editionSelect) || pick.costume < 0 ||
        pick.costume >= CostumeCount(pick.fighter) || pick.color < 0 ||
        pick.color >= ColorCount(pick.fighter, pick.costume)) return false;
    const auto ultras = AllowedUltras(pick.fighter, pick.edition);
    return std::find(ultras.begin(), ultras.end(), pick.ultra) != ultras.end();
}
bool Available(const Pick& pick, bool editionSelect, const Availability& availability) {
    if (!Valid(pick, editionSelect)) return false;
    const auto actions = AllowedPersonalActions(availability);
    if (std::find(actions.begin(), actions.end(), pick.personalAction) == actions.end()) return false;
    const auto colors = AllowedColors(pick.fighter, pick.costume, availability);
    return std::find(colors.begin(), colors.end(), pick.color) != colors.end();
}
bool Normalize(Pick& pick, bool editionSelect, const Availability* availability) {
    const Pick before = pick;
    if (pick.personalAction != 255 && (pick.personalAction < 0 || pick.personalAction > 9)) pick.personalAction = 255;
    if (pick.winQuote != 255 && (pick.winQuote < 0 || pick.winQuote > 10)) pick.winQuote = 255;
    if (pick.handicap < 0 || pick.handicap > 4) pick.handicap = 0;
    if (!FindFighter(pick.fighter)) pick.fighter = 0;
    pick.edition = NormalizeEdition(pick.fighter, pick.edition, editionSelect);
    if (pick.costume < 0 || pick.costume >= CostumeCount(pick.fighter)) pick.costume = 0;
    if (pick.color < 0 || pick.color >= ColorCount(pick.fighter, pick.costume)) pick.color = 0;
    const auto ultras = AllowedUltras(pick.fighter, pick.edition);
    if (std::find(ultras.begin(), ultras.end(), pick.ultra) == ultras.end()) pick.ultra = 0;
    // Unknown availability is not a reason to destroy a saved valid pick.
    if (availability && availability->ready) {
        const auto actions = AllowedPersonalActions(*availability);
        if (std::find(actions.begin(), actions.end(), pick.personalAction) == actions.end()) pick.personalAction = 255;
        const auto costumes = AllowedCostumes(pick.fighter, *availability);
        if (!costumes.empty() && std::find(costumes.begin(), costumes.end(), pick.costume) == costumes.end())
            pick.costume = costumes.front();
        const auto colors = AllowedColors(pick.fighter, pick.costume, *availability);
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
