#include "StageCatalog.hxx"

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
const Stage* FindStage(std::int64_t nativeId) {
    for (const auto& stage : StageList()) if (stage.id == nativeId) return &stage;
    return nullptr;
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
    return FindStage(id) && !(excluded >> id & 1u);
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
