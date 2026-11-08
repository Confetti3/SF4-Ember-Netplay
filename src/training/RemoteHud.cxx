#include "RemoteHud.hxx"
#include "GameFiles.hxx"
#include "../platform/GameThemes.hxx"
#include "../platform/HudLayout.hxx"
#include "../platform/HudTextures.hxx"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
namespace layout = sf4e::platform::hud_layout;
namespace textures = sf4e::platform::hud_textures;

namespace sf4e { namespace training { namespace remote { namespace hud {
namespace {
std::atomic<bool> s_allowed{false};
constexpr std::size_t MostFile = 16u << 20;

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

// A HUD file: the theme's own, else the game's, in the order the game looks (a patch folder beats the base).
const char* const GameFolders[] = { "patch_ae2_tu3", "patch_ae2_tu2", "patch", "dlc/04_ae2", "dlc/03_character_free", "resource" };
fs::path Find(const Host& host, const std::string& name, const std::string& relative) {
    std::error_code ignored;
    const fs::path own = ThemeFolder(host, name) / fs::u8path(relative);
    if (fs::is_regular_file(own, ignored)) return own;
    for (const char* folder : GameFolders) {
        const fs::path path = fs::path(host.directory()) / folder / fs::u8path(relative);
        if (std::string(folder) == "patch_ae2_tu2" && relative.size() >= 15 && relative.compare(relative.size() - 15, 15, "cockpit.ems.emz") == 0) continue;
        if (fs::is_regular_file(path, ignored)) return path;
    }
    return {};
}
bool Read(const fs::path& path, std::string& out) {
    std::vector<std::uint8_t> bytes; std::string error;
    if (path.empty() || !ReadFile(path, MostFile, bytes, error)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}
// The theme's HUD files by name; false without cockpit.
bool Files(const Host& host, const std::string& name, std::map<std::string, std::string>& files) {
    for (const std::string& file : layout::Files()) {
        std::string bytes;
        if (Read(Find(host, name, "battle/hud/CMN/" + file + ".ems.emz"), bytes)) files[file] = std::move(bytes);
    }
    return files.count("cockpit") != 0;
}
std::vector<std::string> Fighters(const Host& host) {
    std::vector<std::string> names; std::error_code ignored;
    for (fs::directory_iterator it(fs::path(host.directory()) / "resource" / "battle" / "hud" / "ENG", ignored), end; !ignored && it != end; it.increment(ignored))
        if (it->is_directory(ignored)) names.push_back(it->path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}
// The texture sheet a HUD file draws from, for a fighter's own parts (chara, uc).
std::string SheetOf(const std::string& file, const std::string& fighter) {
    if (file == "cockpit") return "battle/hud/CMN/cockpit.tex.emz";
    if (file == "cockpit_text") return "battle/hud/ENG/cockpit_text.tex.emz";
    if (file == "chara" || file == "uc") return fighter.empty() ? std::string() : "battle/hud/ENG/" + fighter + "/" + file + ".tex.emz";
    return {};
}
bool Sheet(const Host& host, const std::string& name, const std::string& file, const std::string& fighter, std::vector<std::string>& dds) {
    std::string bytes;
    const std::string relative = SheetOf(file, fighter);
    return !relative.empty() && Read(Find(host, name, relative), bytes) && textures::Entries(bytes, dds);
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
    if (!post && (route == "scene" || route == "sheet")) {
        const std::string name = Param(query, "name"), fighters_ = Param(query, "fighter");
        const auto fighters = Fighters(host);
        const bool known = std::find(fighters.begin(), fighters.end(), fighters_) != fighters.end();
        const std::string fighter = known ? fighters_ : fighters.empty() ? std::string() : (std::find(fighters.begin(), fighters.end(), "RYU") != fighters.end() ? std::string("RYU") : fighters.front());
        std::map<std::string, std::string> files;
        if (!PlainName(name) || !Files(host, name, files)) return Fail(404, "no such theme");
        if (route == "sheet") {
            std::vector<std::string> dds; textures::Image image;
            const int index = std::atoi(Param(query, "index").c_str());
            if (!Sheet(host, name, Param(query, "file"), fighter, dds) || index < 0 || static_cast<std::size_t>(index) >= dds.size() || !textures::Decode(dds[index], image)) return Fail(404, "no such texture");
            Reply r; r.status = 200; r.type = "image/png"; r.body = textures::EncodePng(image);
            return r;
        }
        nlohmann::json sprites = nlohmann::json::array(), sheets = nlohmann::json::object();
        for (const layout::Sprite& sp : layout::Sprites(files)) {
            sprites.push_back({ { "n", sp.node }, { "e", sp.element }, { "f", sp.file }, { "t", sp.tex }, { "uv", { sp.uv[0], sp.uv[1], sp.uv[2], sp.uv[3] } },
                { "r", { sp.r[0], sp.r[1], sp.r[2], sp.r[3] } }, { "p", { sp.pos[0], sp.pos[1], sp.pos[2] } }, { "rot", sp.rot }, { "add", sp.additive } });
        }
        for (const std::string& file : layout::Files()) {
            std::vector<std::string> dds;
            if (!files.count(file) || !Sheet(host, name, file, fighter, dds)) continue;
            nlohmann::json sizes = nlohmann::json::array();
            for (const std::string& d : dds) { std::uint32_t h = 0, w = 0; if (d.size() >= 20) { std::memcpy(&h, d.data() + 12, 4); std::memcpy(&w, d.data() + 16, 4); } sizes.push_back({ w, h }); }
            sheets[file] = sizes;
        }
        return Say(200, { { "fighter", fighter }, { "fighters", fighters }, { "sprites", sprites }, { "sheets", sheets } });
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
