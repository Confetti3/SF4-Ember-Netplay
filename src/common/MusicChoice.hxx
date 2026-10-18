#pragma once
#include "StageCatalog.hxx"
#include "FighterCatalog.hxx"
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace music {
// Stable local preference IDs; never sent to the room or game simulation.
// Preserve the first prototype's native stage IDs. Character IDs use their
// native roster index plus 100, independently of presentation order.
constexpr int Default = -1;
constexpr int Random = -2;
constexpr int CharacterBase = 100;
constexpr int TrackCount = selection::VersusStageCount + selection::FighterCount;
inline bool Valid(std::int64_t choice) {
    return choice == Default || choice == Random || selection::FindStage(choice) ||
        (choice >= CharacterBase && choice < CharacterBase + selection::FighterCount);
}
inline bool ValidLobby(std::int64_t choice) { return Valid(choice) && choice != Random; }
inline int Normalize(std::int64_t choice, bool allowRandom = true) {
    return Valid(choice) && (allowRandom || choice != Random) ? static_cast<int>(choice) : Default;
}
struct Track { int id; const char* name; std::string path; bool character; };
inline const std::vector<Track>& TrackList() {
    static const auto tracks = [] {
        std::vector<Track> result;
        result.reserve(TrackCount);
        for (const auto& stage : selection::StageList())
            result.push_back({stage.id, stage.name, std::string("battle/sound/bgm/BGM_") + stage.code + ".csb", false});
        for (int id = 0; id < selection::FighterCount; ++id) {
            const auto* fighter = selection::FindFighter(id);
            result.push_back({CharacterBase + id, fighter->name,
                std::string("battle/sound/bgm/BGM_") + fighter->code + "_2CH.csb", true});
        }
        return result;
    }();
    return tracks;
}
inline const Track* Find(int choice) {
    for (const auto& track : TrackList()) if (track.id == choice) return &track;
    return nullptr;
}
inline int Step(int choice, int delta, bool allowRandom = false) {
    const auto& tracks = TrackList();
    const int firstTrack = allowRandom ? 2 : 1;
    int index = 0; // Default precedes the first stock stage theme.
    if (allowRandom && choice == Random) index = 1;
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].id == choice) { index = static_cast<int>(i) + firstTrack; break; }
    const auto next = static_cast<std::int64_t>(index) + delta;
    if (next <= 0) return Default;
    if (allowRandom && next == 1) return Random;
    if (next >= static_cast<std::int64_t>(tracks.size()) + firstTrack) return tracks.back().id;
    return tracks[static_cast<std::size_t>(next - firstTrack)].id;
}
// The random preference stays saved as Random. Resolve only once per match,
// from installed banks, and retain that concrete choice across loader calls.
inline int ResolveMatch(int choice, std::uint32_t roll, const std::vector<int>& installed) {
    if (choice == Random) return installed.empty() ? Default : installed[roll % installed.size()];
    for (const auto id : installed) if (id == choice) return choice;
    return Default;
}
} }
