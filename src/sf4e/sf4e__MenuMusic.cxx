#include <windows.h>
#include <cstdint>
#include <cstdio>

#include "../common/StageCatalog.hxx"
#include "sf4e__MenuMusic.hxx"

namespace {
// The menu reads its music's path from this table entry each time it starts it
// (Steam 1.05: RVA 0x66b6f0, which holds "ui/sound/bgm/BGM_MAIN.csb").
const char** Entry() {
    return reinterpret_cast<const char**>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(NULL)) + 0x66b6f0);
}
const char* menuTrack = nullptr;
bool on = false;
}

void sf4e::MenuMusic::Use(bool endless) {
    if (menuTrack && endless == on) return;
    if (!menuTrack) menuTrack = *Entry();
    on = endless;
    if (on) Pick();
    else *Entry() = menuTrack;
}

// The game's lobby themes (Dimps::GameEvents::Network::LobbySoundPlayer): a
// stage of its pool (stages 1-21) or a fighter with a theme in the game's
// files (BGM_<CHR>_2CH.csb), both in one pick.
void sf4e::MenuMusic::Pick() {
    if (!on) return;
    static const char* const fighters[] = {"ADN", "AGL", "BLK", "BLR", "BOS", "BSN", "CDY", "CHB", "CMY", "CNL", "DAN", "DDL",
        "DJY", "DSM", "FLN", "GEN", "GKI", "GKN", "GUL", "GUY", "HKN", "HND", "HWK", "IBK", "JHA", "JRI", "KEN", "MKT", "RIC",
        "ROS", "RYU", "SGT", "SKR", "VEG", "ZGF"};
    static char theme[48];
    const unsigned pick = GetTickCount() % (21 + sizeof(fighters) / sizeof(*fighters));
    if (pick < 21) std::snprintf(theme, sizeof(theme), "battle/sound/bgm/BGM_%s.csb", selection::FindStage(1 + pick)->code);
    else std::snprintf(theme, sizeof(theme), "battle/sound/bgm/BGM_%s_2CH.csb", fighters[pick - 21]);
    *Entry() = theme;
}
