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
} }
