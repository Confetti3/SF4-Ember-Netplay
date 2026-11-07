#include "RemoteHud.hxx"
#include "GameFiles.hxx"
#include "../platform/GameThemes.hxx"
#include "../platform/HudLayout.hxx"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
namespace layout = sf4e::platform::hud_layout;

namespace sf4e { namespace training { namespace remote { namespace hud {
namespace {
std::atomic<bool> s_allowed{false};
constexpr std::size_t MostFile = 4u << 20;

Reply Say(int status, const nlohmann::json& body) { Reply r; r.status = status; r.body = body.dump(); return r; }
Reply Fail(int status, const char* why) { return Say(status, { { "ok", false }, { "error", why } }); }

std::string Decode(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit((unsigned char)text[i + 1]) && std::isxdigit((unsigned char)text[i + 2])) { out += (char)std::stoi(text.substr(i + 1, 2), nullptr, 16); i += 2; }
        else out += text[i] == '+' ? ' ' : text[i];
    }
    return out;
}
std::string Param(const std::string& query, const std::string& key) {
    for (std::size_t at = 0; at < query.size();) {
        const auto end = query.find('&', at), stop = end == std::string::npos ? query.size() : end;
        if (query.compare(at, key.size() + 1, key + "=") == 0) return Decode(query.substr(at + key.size() + 1, stop - at - key.size() - 1));
        at = stop + 1;
    }
    return {};
}
// A theme is one folder name: nothing that leaves ember\themes.
bool PlainName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.size() < 100 && name.find_first_of("\\/:") == std::string::npos;
}
fs::path ThemeFolder(const Host& host, const std::string& name) { return fs::path(host.directory()) / platform::game_themes::Folder / fs::u8path(name); }
fs::path HudFolder(const Host& host, const std::string& name) { return ThemeFolder(host, name) / "battle" / "hud" / "CMN"; }

// The theme's HUD files by name; false without cockpit.
bool Files(const Host& host, const std::string& name, std::map<std::string, std::string>& files) {
    for (const std::string& file : layout::Files()) {
        std::vector<std::uint8_t> bytes; std::string error;
        if (ReadFile(HudFolder(host, name) / (file + ".ems.emz"), MostFile, bytes, error)) files[file] = std::string(bytes.begin(), bytes.end());
    }
    return files.count("cockpit") != 0;
}
layout::Layout Saved(const Host& host, const std::string& name) {
    std::ifstream in(ThemeFolder(host, name) / "layout.json", std::ios::binary);
    return layout::LayoutFromJson(nlohmann::json::parse(in, nullptr, false));
}
bool Save(const Host& host, const std::string& name, const layout::Layout& edits) {
    const fs::path target = ThemeFolder(host, name) / "layout.json";
    fs::path temp = target; temp += ".tmp";
    std::error_code failed;
    {
        std::ofstream out(temp, std::ios::binary);
        out << layout::LayoutToJson(edits).dump(1);
        if (!out) return false;
    }
    fs::remove(target, failed);
    fs::rename(temp, target, failed);
    return !failed;
}
nlohmann::json InfoJson(const layout::Info& i) {
    return { { "id", i.id }, { "label", i.label }, { "parent", i.parent }, { "cx", i.cx }, { "cy", i.cy }, { "w", i.w }, { "h", i.h } };
}
}

void Allow(bool allowed) { s_allowed = allowed; }
bool Allowed() { return s_allowed; }
bool Owns(const std::string& path) { return path.compare(0, 9, "/api/hud/") == 0; }

Reply Handle(const Host& host, bool post, const std::string& path, const std::string& query, const std::string& body, const std::string& userAgent) {
    if (!s_allowed) return Fail(403, "off");
    if (!layout::DesktopAgent(userAgent)) return Fail(403, "desktop");
    const std::string route = path.substr(9);
    if (!post && route == "themes") {
        nlohmann::json names = nlohmann::json::array();
        for (const std::string& name : platform::game_themes::Names(host.directory(), platform::game_themes::BattleHud)) {
            std::error_code ignored;
            if (fs::is_regular_file(HudFolder(host, name) / "cockpit.ems.emz", ignored)) names.push_back(name);
        }
        return Say(200, { { "themes", names } });
    }
    if (!post && route == "theme") {
        const std::string name = Param(query, "name");
        std::map<std::string, std::string> files;
        if (!PlainName(name) || !Files(host, name, files)) return Fail(404, "no such theme");
        nlohmann::json elements = nlohmann::json::array();
        for (const layout::Info& i : layout::Describe(files)) elements.push_back(InfoJson(i));
        return Say(200, { { "name", name }, { "elements", elements }, { "layout", layout::LayoutToJson(Saved(host, name)) } });
    }
    if (post && (route == "layout" || route == "apply")) {
        const auto json = nlohmann::json::parse(body, nullptr, false);
        const std::string name = json.is_object() && json.value("theme", nlohmann::json()).is_string() ? json["theme"].get<std::string>() : std::string();
        std::map<std::string, std::string> files;
        if (!PlainName(name) || !Files(host, name, files)) return Fail(404, "no such theme");
        const bool given = json.contains("layout");
        const layout::Layout edits = given ? layout::LayoutFromJson(json["layout"]) : Saved(host, name);
        if (given && !Save(host, name, edits)) return Fail(500, "cannot save");
        if (route == "layout") return Say(200, { { "ok", true } });
        std::map<std::string, std::string> patched;
        if (!layout::Apply(files, edits, patched)) return Fail(500, "cannot read the theme");
        host.apply(name, std::move(patched));
        return Say(200, { { "ok", true } });
    }
    return Fail(404, "no");
}
} } } }
