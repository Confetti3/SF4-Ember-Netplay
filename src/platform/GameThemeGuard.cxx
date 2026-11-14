#include "GameThemeGuard.hxx"
#include "DurableFile.hxx"
#include "../common/BoundedRead.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace platform { namespace game_themes {
namespace {
constexpr std::size_t MostState = 256 * 1024;

std::map<std::string, std::string> Pairs(const nlohmann::json& json, const char* key) {
    std::map<std::string, std::string> pairs;
    const auto found = json.find(key);
    if (found != json.end() && found->is_object())
        for (const auto& [part, theme] : found->items()) if (theme.is_string() && !theme.get<std::string>().empty()) pairs[part] = theme.get<std::string>();
    return pairs;
}
}

bool Arm(Guard& guard, const std::string& part, const std::string& theme) {
    if (theme.empty()) return false;
    const auto proven = guard.proven.find(part), armed = guard.armed.find(part);
    if ((proven != guard.proven.end() && proven->second == theme) || (armed != guard.armed.end() && armed->second == theme)) return false;
    guard.armed[part] = theme;
    return true;
}

bool Prove(Guard& guard, const std::string& part, const std::string& theme) {
    if (theme.empty()) return false;
    bool changed = guard.armed.erase(part) != 0;
    const auto proven = guard.proven.find(part);
    if (proven == guard.proven.end() || proven->second != theme) { guard.proven[part] = theme; changed = true; }
    const auto closed = guard.closed.find(part);
    if (closed != guard.closed.end() && closed->second.erase(theme)) {
        if (closed->second.empty()) guard.closed.erase(closed);
        changed = true;
    }
    return changed;
}

bool Forget(Guard& guard, const std::string& part) { return guard.proven.erase(part) != 0; }

std::vector<TurnedOff> Recover(Guard& guard) {
    std::vector<TurnedOff> turned;
    for (const auto& [part, theme] : guard.armed) {
        guard.closed[part].insert(theme);
        const auto proven = guard.proven.find(part);
        if (proven != guard.proven.end() && proven->second == theme) guard.proven.erase(proven);
        turned.push_back({ part, theme });
    }
    guard.armed.clear();
    return turned;
}

bool Closed(const Guard& guard, const std::string& part, const std::string& theme) {
    const auto found = guard.closed.find(part);
    return found != guard.closed.end() && found->second.count(theme) != 0;
}

std::string ToJson(const Guard& guard) {
    nlohmann::json json = { { "armed", guard.armed }, { "proven", guard.proven }, { "closed", guard.closed } };
    return json.dump(1);
}

Guard FromJson(const std::string& text) {
    Guard guard;
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (!json.is_object()) return guard;
    guard.armed = Pairs(json, "armed");
    guard.proven = Pairs(json, "proven");
    const auto closed = json.find("closed");
    if (closed != json.end() && closed->is_object())
        for (const auto& [part, themes] : closed->items())
            if (themes.is_array())
                for (const auto& theme : themes) if (theme.is_string() && !theme.get<std::string>().empty()) guard.closed[part].insert(theme.get<std::string>());
    return guard;
}

Guard Load(const std::filesystem::path& gameDirectory) {
    const auto read = durable::ReadBounded(gameDirectory / GuardFile, MostState);
    if (read.status != durable::ReadStatus::Read) return {};
    return FromJson(std::string(read.bytes.begin(), read.bytes.end()));
}

bool Save(const std::filesystem::path& gameDirectory, const Guard& guard) {
    const std::filesystem::path path = gameDirectory / GuardFile;
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    const std::string text = ToJson(guard);
    return static_cast<bool>(durable::PublishReplace(path, durable::PartialPath(path), text.data(), text.size()));
}
} } }
