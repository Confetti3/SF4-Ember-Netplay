#include "StageCatalog.hxx"
#include <algorithm>

#include <cstring>

namespace sf4e { namespace selection {
const std::array<Stage, VersusStageCount>& StageList() {
    // Native code table RVA 0x66b678. Display names match the released USFIV menu.
    static const std::array<Stage, VersusStageCount> stages = {{
        {0, "TRN", "Training Stage"},
        {1, "CHN", "Crowded Downtown"},
        {2, "USA", "Drive-in at Night"},
        {3, "RUS", "Snowy Rail Yard"},
        {4, "BRA", "Inland Jungle"},
        {5, "AFR", "Small Airfield"},
        {6, "VIE", "Beautiful Bay"},
        {7, "EUR", "Cruise Ship Stern"},
        {8, "RVR", "Overpass"},
        {9, "VCN", "Volcanic Rim"},
        {10, "SCO", "Historic Distillery"},
        {11, "JPN", "Old Temple"},
        {12, "LAB", "Secret Laboratory"},
        {13, "IND", "Exciting Street Scene"},
        {14, "KOR", "Festival at the Old Temple"},
        {15, "BLD", "Skyscraper Under Construction"},
        {16, "CNX", "Run-down Back Alley"},
        {17, "BRX", "Pitch-black Jungle"},
        {18, "VNX", "Morning Mist Bay"},
        {19, "JPX", "Deserted Temple"},
        {20, "AFX", "Solar Eclipse"},
        {21, "LBX", "Crumbling Laboratory"},
        {24, "DET", "Pit Stop 109"},
        {25, "ELV", "Cosmic Elevator"},
        {26, "HFP", "Half Pipe"},
        {27, "MAD", "Mad Gear Hideout"},
        {28, "BFU", "Blast Furnace"},
        {29, "JUR", "Jurassic Era Research Facility"}
    }};
    return stages;
}
namespace {
// Custom stages fall back to these in turn: a letter and a number (C12, D05) by its number, 1 CHN, 2 RUS ...
// 18 HFP, 19 CHN ...; any other code by the sum of its characters. Never BLD, LBX, USA, MAD, BFU, LAB, VCN, JUR,
// TRN or DET.
constexpr int customFallbacks[] = {1, 3, 4, 5, 6, 7, 8, 10, 11, 13, 14, 16, 17, 18, 19, 20, 25, 26};
constexpr int FallbackCount = sizeof(customFallbacks) / sizeof(customFallbacks[0]);
// The codes the game has besides the versus stages: the bonus rounds (native ids 22, 23).
const char* const bonusCodes[] = {"GAS", "SCX"};
bool CodeCharacter(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
bool GameCode(const char* code) {
    for (const auto& stage : StageList()) if (std::strcmp(stage.code, code) == 0) return true;
    for (const char* bonus : bonusCodes) if (std::strcmp(bonus, code) == 0) return true;
    return false;
}
}
int CustomStageId(const char* code) {
    if (!code || std::strlen(code) != 3 || !CodeCharacter(code[0]) || !CodeCharacter(code[1]) || !CodeCharacter(code[2]) || GameCode(code)) return -1;
    return code[0] << 16 | code[1] << 8 | code[2];
}
bool IsCustomStage(std::int64_t id) {
    if (id < 0 || id > 0xffffff) return false;
    const char code[4] = {static_cast<char>(id >> 16), static_cast<char>(id >> 8 & 0xff), static_cast<char>(id & 0xff), 0};
    return CustomStageId(code) == id;
}
const Stage* FindStage(std::int64_t nativeId) {
    for (const auto& stage : StageList()) if (stage.id == nativeId) return &stage;
    return nullptr;
}
bool ValidStage(std::int64_t id) { return FindStage(id) != nullptr || IsCustomStage(id); }
std::string StageCode(std::int64_t id) {
    if (const Stage* stage = FindStage(id)) return stage->code;
    if (!IsCustomStage(id)) return std::string();
    const char code[4] = {static_cast<char>(id >> 16), static_cast<char>(id >> 8 & 0xff), static_cast<char>(id & 0xff), 0};
    return code;
}
int CustomStageFallback(std::int64_t id) {
    if (!IsCustomStage(id)) return NormalizeStage(id);
    const int a = static_cast<int>(id >> 16), b = static_cast<int>(id >> 8 & 0xff), c = static_cast<int>(id & 0xff);
    const int number = (b >= '0' && b <= '9' && c >= '0' && c <= '9') ? (b - '0') * 10 + (c - '0') : 0;
    const bool numbered = a >= 'A' && a <= 'Z' && number > 0;
    return customFallbacks[(numbered ? number - 1 : a + b + c) % FallbackCount];
}
int NormalizeStage(std::int64_t nativeId) {
    return ValidStage(nativeId) ? static_cast<int>(nativeId) : 0;
}
bool IsRandomStage(std::int64_t id) { return id == RandomStageId; }
int NormalizeStageChoice(std::int64_t id) {
    return IsRandomStage(id) ? RandomStageId : NormalizeStage(id);
}
RandomPool::RandomPool(const std::vector<std::int64_t>& excluded) {
    for (std::int64_t id : excluded)
        if (ValidStage(id)) excluded_.push_back(static_cast<int>(id));
    std::sort(excluded_.begin(), excluded_.end());
    excluded_.erase(std::unique(excluded_.begin(), excluded_.end()), excluded_.end());
    // Every game stage taken out leaves Random nowhere it can always go: they all come back.
    if (StockIn() == 0)
        excluded_.erase(std::remove_if(excluded_.begin(), excluded_.end(), [](int id) { return FindStage(id) != nullptr; }), excluded_.end());
}
int RandomPool::StockIn() const {
    int in = 0;
    for (const auto& stage : StageList()) in += !std::binary_search(excluded_.begin(), excluded_.end(), stage.id);
    return in;
}
bool RandomPool::Contains(int id) const {
    return ValidStage(id) && !std::binary_search(excluded_.begin(), excluded_.end(), id);
}
bool RandomPool::CanToggle(int id) const {
    if (!ValidStage(id)) return false;
    return !(FindStage(id) && Contains(id) && StockIn() == 1);
}
bool RandomPool::Toggle(int id) {
    if (!CanToggle(id)) return false;
    const auto at = std::lower_bound(excluded_.begin(), excluded_.end(), id);
    if (at != excluded_.end() && *at == id) excluded_.erase(at);
    else excluded_.insert(at, id);
    return true;
}
std::vector<int> RandomPool::Stages(const std::vector<int>& installedCustoms) const {
    std::vector<int> in;
    for (const auto& stage : StageList()) if (Contains(stage.id)) in.push_back(stage.id);
    for (int id : installedCustoms) if (IsCustomStage(id) && Contains(id)) in.push_back(id);
    return in;
}
int ResolveStage(int choice, std::uint32_t roll, const RandomPool& pool, const std::vector<int>& installedCustoms) {
    if (!IsRandomStage(choice)) return NormalizeStage(choice);
    const std::vector<int> stages = pool.Stages(installedCustoms);
    return stages[roll % static_cast<std::uint32_t>(stages.size())];
}
} }
