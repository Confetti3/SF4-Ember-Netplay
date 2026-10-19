#pragma once
#include "MusicChoice.hxx"
#include <string>

namespace sf4e { namespace music {
// Native BgmPlayer constructor mode: 1 allocates three synchronized cue
// players and crossfades between them; 0 allocates one. Every stock stage
// bank has cues 0/1/2, but every stock character stereo bank has only cue 0.
// Match the replacement bank, rather than the original arena's bank layout.
inline int BattleCueMode(int resolvedChoice, int nativeMode) {
    return resolvedChoice >= CharacterBase && resolvedChoice < CharacterBase + selection::FighterCount ? 0 :
        ValidLobby(resolvedChoice) && resolvedChoice != Default ? 1 : nativeMode;
}
// An installed custom stage owns its bank and its native cue mode.
inline int BattleChoice(int selected, bool customStageMusic) { return customStageMusic ? Default : selected; }
// Root-event names from the game's flow table. Battle-bearing modes and
// an empty foreground during transitions must never start menu audio.
// LocalBattleLog also owns replay battles, so it is not a UI-only menu.
// Room membership is irrelevant: the saved theme applies from Title onward.
inline bool MenuPlaybackAllowed(const std::string& foreground, bool battleBusy) {
    if (battleBusy) return false;
    for (const auto menu : {"Title", "MainMenu", "Option", "PlayerData",
                           "PlayerTitleSetting", "PlayerIconSetting", "Manual", "Marketplace"})
        if (foreground == menu) return true;
    return false;
}
// A menu bank may only be replaced or started at a safe native menu.
// Remember it across loading/fights so it is restored on the return to menus.
struct MenuPlaybackState {
    int applied = Default;
    bool pending = false;
    bool retry = false;
    bool Request(int wanted, bool safeMenu) {
        if (!safeMenu || (applied == wanted && !retry)) return false;
        retry = false;
        applied = wanted;
        pending = true;
        return true;
    }
    void Retry() { retry = true; }
    void Reset() { applied = Default; pending = false; retry = false; }
};
} }
