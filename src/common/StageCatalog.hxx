#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace sf4e { namespace selection {
constexpr int VersusStageCount = 28;
struct Stage { int id; const char* code; const char* name; };
// IDs are native table offsets, not grid positions. 22/23 are bonus rounds.
const std::array<Stage, VersusStageCount>& StageList();
const Stage* FindStage(std::int64_t nativeId);      // also knows the custom stages
int NormalizeStage(std::int64_t nativeId);
// Custom stages, added by a separate setup program (Ember never downloads anything): any three-character code of
// capital letters and digits that isn't one of the game's (C12, D05, XYZ ...), its files battle\stage\STG_<code>.emz.
// The code is the stage's id, its characters packed ('C' << 16 | '1' << 8 | '2'), so a custom stage travels as one
// number like any other. A PC without it shows its fallback, a stock stage chosen by rule. Random can land on one P1
// has installed (P1 resolves Random), and the other side then sees it or its fallback, as if it had been picked.
int CustomStageId(const char* code);   // -1 for anything but a custom code
bool IsCustomStage(std::int64_t id);
int CustomStageFallback(std::int64_t id);
// The name the setup program gives a custom stage installed on this PC (sf4e::custom reads it), instead of
// "Custom stage C12". Display only, at most 63 bytes.
void SetCustomStageName(std::int64_t id, const char* name);
// A local choice only, like the 255 sentinel used for personal action and win
// quote. P1 resolves it to a catalog stage before the stage is sent, so it is
// never valid on the wire and FindStage, NormalizeStage and ReadStage reject it.
constexpr int RandomStageId = 255;
bool IsRandomStage(std::int64_t id);
// A catalog id or RandomStageId stays; anything else becomes 0.
int NormalizeStageChoice(std::int64_t id);
// Stages the player took out of Random, one bit per catalog id (all ids are
// below 32). A local choice like Random itself: P1 applies it when resolving.
using StageMask = std::uint32_t;
bool InRandomPool(int id, StageMask excluded);
int RandomPoolSize(StageMask excluded);
// Keeps catalog bits only, and puts every stage back when none would be left.
StageMask NormalizeRandomExclusions(std::uint64_t excluded);
// Custom stages the player took out of Random, by id, earliest first, 0 after the last: a fixed size, so the saved
// preferences stay plain data. An installed custom stage is in Random unless listed (one installed again keeps the
// choice); when the list is full, taking one more out puts the earliest back.
using CustomStageExclusions = std::array<std::int32_t, 64>;
bool CustomStageExcluded(int id, const CustomStageExclusions& excluded);
void ExcludeCustomStage(int id, bool out, CustomStageExclusions& excluded);
// The installed custom stages (ids) that are in Random.
std::vector<int> CustomStagesInRandom(const std::vector<int>& installed, const CustomStageExclusions& excluded);
// Random becomes the stage picked by roll among the game's stages not excluded
// and the custom stages given; any other choice is normalized.
int ResolveStage(int choice, std::uint32_t roll, StageMask excluded = 0, const std::vector<int>& customs = {});
} }
