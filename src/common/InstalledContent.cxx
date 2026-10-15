#include "InstalledContent.hxx"

#include <algorithm>

namespace sf4e { namespace selection {
namespace {
const std::vector<int>& None() { static const std::vector<int> none; return none; }
bool Has(const std::vector<int>& list, int value) { return std::binary_search(list.begin(), list.end(), value); }
}

const std::vector<int>& InstalledContent::Costumes(int fighterId) const {
    return fighterId >= 0 && fighterId < FighterCount ? costumes[fighterId] : None();
}
const std::vector<int>& InstalledContent::Colors(int fighterId, int costumeId) const {
    const auto found = colors.find(std::make_pair(fighterId, costumeId));
    return found == colors.end() ? None() : found->second;
}
bool InstalledContent::HasCostume(int fighterId, int costumeId) const { return Has(Costumes(fighterId), costumeId); }
bool InstalledContent::HasColor(int fighterId, int costumeId, int colorId) const { return Has(Colors(fighterId, costumeId), colorId); }
bool InstalledContent::HasStage(int stageId) const { return Has(stages, stageId); }
std::string InstalledContent::Name(int fighterId, int costumeId, int colorId) const {
    const auto found = names.find(std::make_tuple(fighterId, costumeId, colorId));
    return found == names.end() ? std::string() : found->second;
}
std::string InstalledContent::StageName(int stageId) const {
    const auto found = stageNames.find(stageId);
    return found == stageNames.end() ? std::string() : found->second;
}

Appearance ResolveAppearance(const Look (&picks)[2], const InstalledContent& installed) {
    Appearance result;
    for (int i = 0; i < 2; i++) {
        Look& look = result.shown[i];
        look = picks[i];
        if (IsCustomCostume(look.costume) && !installed.HasCostume(look.fighter, look.costume)) {
            look.costume = 0;
            look.color = 0;
        } else if (IsCustomColor(look.color) && !installed.HasColor(look.fighter, look.costume, look.color)) {
            look.color = 0;
        }
    }
    // The two sides never look the same: when they would (a mirror on the same costume and color, as picked or after
    // falling back), the second side takes color 1, or color 2 when the first is in color 1.
    if (result.shown[0] == result.shown[1]) result.shown[1].color = result.shown[0].color == 0 ? 1 : 0;
    // The stand-ins, side by side: the second sees the first's.
    for (int i = 0; i < 2; i++) {
        Look& look = result.shown[i];
        const Look& other = result.shown[1 - i];
        if (IsCustomCostume(look.costume))
            for (int standIn = 0; standIn < 3; standIn++)
                if (other.fighter != look.fighter || other.costume != standIn) {
                    result.costumes[i] = CostumeSwap(look.fighter, standIn, look.costume);
                    look.costume = standIn;
                    break;
                }
        // then a custom color, on the costume the game sees (a custom costume's stand-in)
        if (IsCustomColor(look.color))
            for (int standIn = 0; standIn < 3; standIn++)
                if (other.fighter != look.fighter || other.costume != look.costume || other.color != standIn) {
                    result.colors[i] = ColorSwap(look.fighter, look.costume, standIn, look.color);
                    look.color = standIn;
                    break;
                }
    }
    return result;
}

} }
