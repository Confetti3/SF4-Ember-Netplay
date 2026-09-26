#pragma once
#include <array>
#include <cstdint>

namespace sf4e { namespace selection {
constexpr int VersusStageCount = 28;
struct Stage { int id; const char* code; const char* name; };
// IDs are native table offsets, not grid positions. 22/23 are bonus rounds.
const std::array<Stage, VersusStageCount>& StageList();
const Stage* FindStage(std::int64_t nativeId);
int NormalizeStage(std::int64_t nativeId);
// A local choice only, like the 255 sentinel used for personal action and win
// quote. P1 resolves it to a catalog stage before the stage is sent, so it is
// never valid on the wire and FindStage, NormalizeStage and ReadStage reject it.
constexpr int RandomStageId = 255;
bool IsRandomStage(std::int64_t id);
// A catalog id or RandomStageId stays; anything else becomes 0.
int NormalizeStageChoice(std::int64_t id);
// Random becomes the catalog stage picked by roll; any other choice is normalized.
int ResolveStage(int choice, std::uint32_t roll);
} }
