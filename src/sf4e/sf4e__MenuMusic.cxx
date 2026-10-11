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

// A stage of the game's lobby pool (stages 1-21, as
// Dimps::GameEvents::Network::LobbySoundPlayer picks them).
void sf4e::MenuMusic::Pick() {
    if (!on) return;
    static char theme[48];
    std::snprintf(theme, sizeof(theme), "battle/sound/bgm/BGM_%s.csb", selection::FindStage(1 + GetTickCount() % 21)->code);
    *Entry() = theme;
}
