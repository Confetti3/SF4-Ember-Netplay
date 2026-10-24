#pragma once
#include "StageCatalog.hxx"
#include "FighterCatalog.hxx"
#include <cstdint>
#include <algorithm>
#include <memory>
#include <cstdlib>
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
inline bool ValidLobby(std::int64_t choice) { return Valid(choice); }
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
// Build this once from the inventory. Presentation and keyboard/controller
// stepping use exactly the same installed, canonical order.
struct Choices {
    std::vector<int> ids{Default, Random};
    explicit Choices(const std::vector<int>& installed = {}) {
        for (const auto& track : TrackList())
            if (std::find(installed.begin(), installed.end(), track.id) != installed.end()) ids.push_back(track.id);
    }
    std::size_t Index(int choice) const {
        const auto it = std::find(ids.begin(), ids.end(), choice);
        return it == ids.end() ? 0 : static_cast<std::size_t>(it - ids.begin());
    }
    int Step(int choice, int delta) const {
        const auto next = static_cast<std::int64_t>(Index(choice)) + delta;
        return ids[static_cast<std::size_t>((std::max)(std::int64_t(0), (std::min)(next, static_cast<std::int64_t>(ids.size() - 1))))];
    }
    int FromKey(const std::string& key) const {
        if (key.empty()) return Default;
        char* end = nullptr;
        const auto value = std::strtol(key.c_str(), &end, 10);
        if (end != key.c_str() + key.size() || !Valid(value)) return Default;
        return ids[Index(static_cast<int>(value))];
    }
};
using ChoicesPtr = std::shared_ptr<const Choices>;
// The random preference stays saved as Random. Resolve only once per match,
// from installed banks, and retain that concrete choice across loader calls.
inline int ResolveMatch(int choice, std::uint32_t roll, const std::vector<int>& installed) {
    if (choice == Random) return installed.empty() ? Default : installed[roll % installed.size()];
    for (const auto id : installed) if (id == choice) return choice;
    return Default;
}
} }
