#pragma once
#include <string>
#include "FighterCatalog.hxx"
#include "StageCatalog.hxx"

namespace sf4e { namespace lobby_music {

// The themes the game's online lobby plays (Dimps::GameEvents::Network::LobbySoundPlayer): a
// stage of its pool, or a fighter's theme. The pool is the native stages 1 to 21, the lobby's
// own count and deliberately not selection::VersusStageCount, which also counts the Training
// stage and the stages the game added later.
constexpr unsigned LobbyStageCount = 21;
// One roll per stage of the pool, then one per fighter of the catalog (the game ships
// BGM_<CHR>_2CH.csb for every one of them).
constexpr unsigned RollCount = LobbyStageCount + static_cast<unsigned>(selection::FighterCount);

// The path the menu loads for a roll; a roll past RollCount wraps, so any random number will do.
inline std::string Path(unsigned roll) {
    roll %= RollCount;
    const std::string folder = "battle/sound/bgm/BGM_";
    if (roll < LobbyStageCount) return folder + selection::FindStage(1 + roll)->code + ".csb";
    return folder + selection::FindFighter(static_cast<int>(roll - LobbyStageCount))->code + "_2CH.csb";
}

} }
