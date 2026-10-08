#include "StageCatalog.hxx"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

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
// Every code the game has (native table RVA 0x66b678, ids 0..29, the bonus rounds GAS and SCX included).
const char* const gameCodes[] = {"TRN", "CHN", "USA", "RUS", "BRA", "AFR", "VIE", "EUR", "RVR", "VCN", "SCO", "JPN", "LAB",
    "IND", "KOR", "BLD", "CNX", "BRX", "VNX", "JPX", "AFX", "LBX", "GAS", "SCX", "DET", "ELV", "HFP", "MAD", "BFU", "JUR"};
bool CodeCharacter(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
// A lasting Stage for any custom id, made the first time it is asked for.
const Stage* CustomStage(std::int64_t id) {
    if (!IsCustomStage(id)) return nullptr;
    struct Entry { char code[4]; char name[24]; Stage stage; };
    static std::mutex lock;
    static std::map<std::int64_t, std::unique_ptr<Entry>> made;
    std::lock_guard<std::mutex> hold(lock);
    auto& entry = made[id];
    if (!entry) {
        entry.reset(new Entry());
        entry->code[0] = static_cast<char>(id >> 16);
        entry->code[1] = static_cast<char>(id >> 8 & 0xff);
        entry->code[2] = static_cast<char>(id & 0xff);
        std::snprintf(entry->name, sizeof(entry->name), "Custom stage %s", entry->code);
        entry->stage = {static_cast<int>(id), entry->code, entry->name};
    }
    return &entry->stage;
}
}
int CustomStageId(const char* code) {
    if (!code || std::strlen(code) != 3 || !CodeCharacter(code[0]) || !CodeCharacter(code[1]) || !CodeCharacter(code[2])) return -1;
    for (const char* game : gameCodes) if (std::strcmp(game, code) == 0) return -1;
    return code[0] << 16 | code[1] << 8 | code[2];
}
bool IsCustomStage(std::int64_t id) {
    if (id < 0 || id > 0xffffff) return false;
    const char code[4] = {static_cast<char>(id >> 16), static_cast<char>(id >> 8 & 0xff), static_cast<char>(id & 0xff), 0};
    return CustomStageId(code) == id;
}
const Stage* FindStage(std::int64_t nativeId) {
    if (IsCustomStage(nativeId)) return CustomStage(nativeId);
    for (const auto& stage : StageList()) if (stage.id == nativeId) return &stage;
    return nullptr;
}
int CustomStageFallback(std::int64_t id) {
    if (!IsCustomStage(id)) return NormalizeStage(id);
    const int a = static_cast<int>(id >> 16), b = static_cast<int>(id >> 8 & 0xff), c = static_cast<int>(id & 0xff);
    const int number = (b >= '0' && b <= '9' && c >= '0' && c <= '9') ? (b - '0') * 10 + (c - '0') : 0;
    const bool numbered = a >= 'A' && a <= 'Z' && number > 0;
    return customFallbacks[(numbered ? number - 1 : a + b + c) % FallbackCount];
}
int NormalizeStage(std::int64_t nativeId) {
    const auto* stage = FindStage(nativeId);
    return stage ? stage->id : 0;
}
bool IsRandomStage(std::int64_t id) { return id == RandomStageId; }
int NormalizeStageChoice(std::int64_t id) {
    return IsRandomStage(id) ? RandomStageId : NormalizeStage(id);
}
bool InRandomPool(int id, StageMask excluded) {
    return !IsCustomStage(id) && FindStage(id) && !(excluded >> id & 1u);
}
int RandomPoolSize(StageMask excluded) {
    int size = 0;
    for (const auto& stage : StageList()) size += InRandomPool(stage.id, excluded);
    return size;
}
StageMask NormalizeRandomExclusions(std::uint64_t excluded) {
    StageMask known = 0;
    for (const auto& stage : StageList()) known |= StageMask(1) << stage.id;
    const StageMask kept = static_cast<StageMask>(excluded) & known;
    return kept == known ? 0 : kept;
}
int ResolveStage(int choice, std::uint32_t roll, StageMask excluded) {
    if (!IsRandomStage(choice)) return NormalizeStage(choice);
    excluded = NormalizeRandomExclusions(excluded);
    int pick = static_cast<int>(roll % static_cast<std::uint32_t>(RandomPoolSize(excluded)));
    for (const auto& stage : StageList())
        if (InRandomPool(stage.id, excluded) && pick-- == 0) return stage.id;
    return StageList()[0].id;
}
} }
