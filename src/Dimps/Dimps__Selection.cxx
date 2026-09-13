#include "Dimps__Selection.hxx"

namespace Dimps { namespace Selection {
namespace {
std::uint32_t** costumeManager = nullptr;
void* (__cdecl* getUserApp)() = nullptr;
int (__thiscall* colorUnlocked)(void*, int, int) = nullptr;
int (__thiscall* actionUnlocked)(void*, int, int) = nullptr;
}

void Locate(HMODULE executable) {
    const auto base = reinterpret_cast<std::uintptr_t>(executable);
    // USFIV Steam 1.05. The menu's costume predicate at RVA 0x29ff90 reads
    // these per-character masks; the customization menu at RVA 0x2ec64 reads
    // color unlocks from the profile using the predicate at RVA 0x29a9d0.
    costumeManager = reinterpret_cast<std::uint32_t**>(base + 0x6ac99c);
    getUserApp = reinterpret_cast<void* (__cdecl*)()>(base + 0xcf60);
    colorUnlocked = reinterpret_cast<int (__thiscall*)(void*, int, int)>(base + 0x29a9d0);
    actionUnlocked = reinterpret_cast<int (__thiscall*)(void*, int, int)>(base + 0x29aa40);
}

std::uint32_t CostumeAvailabilityMask(std::uint32_t nativeFlags) {
    // Steam 1.05: 69FBD0 -> 69FFD0 validates saved/match costumes against
    // the low ownership byte (also used by 684750 and 6A0820). The high
    // menu mask can omit owned DLC while Ember is open at the main menu.
    return nativeFlags & 0xff;
}

sf4e::selection::Availability ReadAvailability(int fighterId) {
    using namespace sf4e::selection;
    Availability result;
    if (!FindFighter(fighterId) || !costumeManager || !*costumeManager || !getUserApp || !colorUnlocked || !actionUnlocked) return result;
    const auto* app = static_cast<const unsigned char*>(getUserApp());
    if (!app) return result;
    auto* profile = *reinterpret_cast<unsigned char* const*>(app + 0x6c);
    if (!profile) return result;
    result.costumes = CostumeAvailabilityMask((*costumeManager)[fighterId + 1]);
    for (int action = 0; action < 10; ++action)
        if (actionUnlocked(profile + 0x6428, fighterId, action)) result.personalActions |= 1u << action;
    const int base = BaseCostumeCount(fighterId);
    for (int costume = 0; costume < CostumeCount(fighterId); ++costume) {
        if (!(result.costumes & (1u << costume))) continue;
        for (int color = 0; color < ColorCount(fighterId, costume); ++color) {
            const bool unlocked = costume >= base || (costume != 0 && color < 10) ||
                                  colorUnlocked(profile + 0x6428, fighterId, color) != 0;
            if (unlocked) result.colors[costume] |= 1u << color;
        }
    }
    result.ready = true;
    return result;
}
} }
