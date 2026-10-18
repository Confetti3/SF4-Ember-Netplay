#pragma once
#include "MusicChoice.hxx"

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
// A lobby bank may only be replaced or started at a safe native menu.
// Remember it across loading/fights so it is restored on the return to menus.
struct LobbyPlaybackState {
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
