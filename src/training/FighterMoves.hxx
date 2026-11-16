#pragma once
#include <cstring>
#include <vector>

// The moves the dummy's reply can be picked from: the same for every fighter
// (UniversalMoves), then the fighter's own from its command list
// (FighterMoves.inc, made by scripts/gen-fighter-moves.py from scripts/fixtures/usf4-command-moves.tsv).
namespace sf4e { namespace training {
struct FighterMove { const char* code; const char* name; const char* moves; };
struct UniversalMove { const char* name; const char* moves; };
constexpr UniversalMove UniversalMoves[] = {
    {"Back dash", "44"}, {"Forward dash", "66"}, {"Throw", "LP+LK"}, {"Back throw", "4LP+LK"}, {"Delayed throw", "LP+LK@+10"},
    {"Jump up", "8"}, {"Jump forward", "9"}, {"Jump back", "7"}, {"Taunt", "HP+HK"},
    {"Focus attack", "MP+MK"}, {"Red Focus", "LP+MP+MK"},
    {"Close LP", "cl.LP"}, {"Close MP", "cl.MP"}, {"Close HP", "cl.HP"}, {"Close LK", "cl.LK"}, {"Close MK", "cl.MK"}, {"Close HK", "cl.HK"},
    {"Far LP", "far.LP"}, {"Far MP", "far.MP"}, {"Far HP", "far.HP"}, {"Far LK", "far.LK"}, {"Far MK", "far.MK"}, {"Far HK", "far.HK"},
    {"Crouching LP", "2LP"}, {"Crouching MP", "2MP"}, {"Crouching HP", "2HP"}, {"Crouching LK", "2LK"}, {"Crouching MK", "2MK"}, {"Crouching HK", "2HK"},
    {"Jumping LP", "9LP"}, {"Jumping MP", "9MP"}, {"Jumping HP", "9HP"}, {"Jumping LK", "9LK"}, {"Jumping MK", "9MK"}, {"Jumping HK", "9HK"},
};
constexpr FighterMove FighterMoves[] = {
#include "FighterMoves.inc"
};
// The fighter's own moves, in the command list's order (null code: none).
inline std::vector<const FighterMove*> MovesOf(const char* code) {
    std::vector<const FighterMove*> list;
    if (code) for (const auto& move : FighterMoves) if (std::strcmp(move.code, code) == 0) list.push_back(&move);
    return list;
}
} }
