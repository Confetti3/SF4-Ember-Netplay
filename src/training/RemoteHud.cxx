#include "RemoteHud.hxx"
#include "GameFiles.hxx"
#include "../platform/DurableFile.hxx"
#include "../platform/GameThemes.hxx"
#include "../platform/HudLayout.hxx"
#include "../platform/HudTextures.hxx"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <set>
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
fs::path Find(const Host& host, const std::string& name, const std::string& relative) {
    std::error_code ignored;
    const fs::path own = ThemeFolder(host, name) / fs::u8path(relative);
    if (fs::is_regular_file(own, ignored)) return own;
    for (const char* folder : GameLayers) {
        const fs::path path = fs::path(host.directory()) / folder / fs::u8path(relative);
        // Carried over from the lab editor, which never takes cockpit.ems.emz from patch_ae2_tu2.
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
// Pictures the user put over parts of the sheets: <theme>\layout-art\<file>[.<FIGHTER>]-<tex>-<u0>-<v0>-<u1>-<v1>.png
struct Art { std::string file, fighter; int tex = 0, u[4] = {}; };
std::string ArtName(const Art& a) {
    return a.file + (a.fighter.empty() ? "" : "." + a.fighter) + "-" + std::to_string(a.tex) + "-" + std::to_string(a.u[0]) + "-" + std::to_string(a.u[1]) + "-" + std::to_string(a.u[2]) + "-" + std::to_string(a.u[3]);
}
bool Valid(const Art& a) {
    const bool own = a.file == "chara" || a.file == "uc";
    if (!(own || a.file == "cockpit" || a.file == "cockpit_text") || own != !a.fighter.empty() || (own && !PlainName(a.fighter))) return false;
    return a.tex >= 0 && a.tex <= 40 && a.u[0] >= 0 && a.u[1] >= 0 && a.u[0] < a.u[2] && a.u[1] < a.u[3] && a.u[2] <= 4096 && a.u[3] <= 4096 && a.u[2] - a.u[0] <= 1024 && a.u[3] - a.u[1] <= 1024;
}
bool ParseArt(const std::string& stem, Art& a) {
    std::vector<std::string> parts; std::size_t at = 0;
    for (;;) { const auto dash = stem.find('-', at); parts.push_back(stem.substr(at, dash == std::string::npos ? std::string::npos : dash - at)); if (dash == std::string::npos) break; at = dash + 1; }
    if (parts.size() != 6) return false;
    const auto dot = parts[0].find('.');
    a.file = parts[0].substr(0, dot); a.fighter = dot == std::string::npos ? std::string() : parts[0].substr(dot + 1);
    int* number[5] = { &a.tex, &a.u[0], &a.u[1], &a.u[2], &a.u[3] };
    for (int i = 0; i < 5; ++i) {
        const std::string& t = parts[i + 1];
        if (t.empty() || t.size() > 5 || t.find_first_not_of("0123456789") != std::string::npos) return false;
        *number[i] = std::atoi(t.c_str());
    }
    return Valid(a);
}
fs::path ArtFolder(const Host& host, const std::string& name) { return ThemeFolder(host, name) / "layout-art"; }
std::vector<Art> ArtOf(const Host& host, const std::string& name) {
    std::vector<Art> list; std::error_code ignored;
    for (fs::directory_iterator it(ArtFolder(host, name), ignored), end; !ignored && it != end; it.increment(ignored)) {
        Art a;
        if (it->path().extension() == ".png" && ParseArt(it->path().stem().string(), a)) list.push_back(a);
    }
    std::sort(list.begin(), list.end(), [](const Art& x, const Art& y) { return ArtName(x) < ArtName(y); });
    return list;
}
bool ReadArt(const Host& host, const std::string& name, const Art& a, textures::Image& image) {
    std::string bytes;
    return Read(ArtFolder(host, name) / (ArtName(a) + ".png"), bytes) && textures::DecodePng(bytes, image) && image.width == a.u[2] - a.u[0] && image.height == a.u[3] - a.u[1];
}
std::string Unbase64(const std::string& text) {
    std::string out; int bits = 0, value = 0;
    for (const char c : text) {
        const char* at = std::strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", c);
        if (!at || !c) continue;
        value = value << 6 | static_cast<int>(at - "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
        if ((bits += 6) >= 8) { bits -= 8; out.push_back(static_cast<char>(value >> bits & 255)); }
    }
    return out;
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
bool WriteFile(const fs::path& path, const std::string& bytes) {
    std::error_code ignored; fs::create_directories(path.parent_path(), ignored);
    return static_cast<bool>(durable::PublishReplace(path, durable::PartialPath(path), bytes.data(), bytes.size()));
}
bool Save(const Host& host, const std::string& name, const layout::Layout& edits) {
    return WriteFile(ThemeFolder(host, name) / "layout.json", layout::LayoutToJson(edits).dump(1));
}
// A sheet with the theme's pictures for it pasted in, as the game will read it.
std::string WithArt(const Host& host, const std::string& name, const std::string& relative, std::string sheet) {
    for (const Art& a : ArtOf(host, name)) {
        textures::Image picture; std::string pasted;
        if (SheetOf(a.file, a.fighter) == relative && ReadArt(host, name, a, picture) && textures::Paste(sheet, a.tex, picture, a.u[0], a.u[1], pasted)) sheet = std::move(pasted);
    }
    return sheet;
}
// The theme's HUD files with the layout and the pictures in them, by path under battle\hud; false when a file cannot be read.
bool Build(const Host& host, const std::string& name, const std::map<std::string, std::string>& files, const layout::Layout& edits, std::map<std::string, std::string>& written) {
    std::map<std::string, std::string> patched;
    written.clear();
    if (!layout::Apply(files, edits, patched)) return false;
    for (auto& [file, bytes] : patched) written["CMN/" + file + ".ems.emz"] = std::move(bytes);
    // The pictures go into the sheets they belong to.
    std::set<std::string> sheets;
    for (const Art& a : ArtOf(host, name)) sheets.insert(SheetOf(a.file, a.fighter));
    for (const std::string& relative : sheets) {
        std::string sheet;
        if (Read(Find(host, name, relative), sheet)) written[relative.substr(11)] = WithArt(host, name, relative, std::move(sheet));      // under battle\hud
    }
    return true;
}
nlohmann::json InfoJson(const layout::Info& i) {
    return { { "id", i.id }, { "label", i.label }, { "parent", i.parent }, { "cx", i.cx }, { "cy", i.cy }, { "w", i.w }, { "h", i.h } };
}

struct Request { const Host& host; std::string query; nlohmann::json json; };      // json: the body of a POST
std::string Text(const nlohmann::json& json, const char* key) {
    return json.is_object() && json.value(key, nlohmann::json()).is_string() ? json[key].get<std::string>() : std::string();
}
// The theme a request names, with its HUD files.
struct Theme { std::string name; std::map<std::string, std::string> files; };
bool OpenTheme(const Host& host, const std::string& name, Theme& theme) { theme.name = name; return PlainName(name) && Files(host, name, theme.files); }
bool PostedTheme(const Request& r, Theme& theme) { return OpenTheme(r.host, Text(r.json, "theme"), theme); }
// The fighter whose names and portraits show: the one asked for, else Ryu, else the first.
std::string FighterOf(const Host& host, const std::string& asked) {
    const auto fighters = Fighters(host);
    const auto has = [&](const std::string& n) { return std::find(fighters.begin(), fighters.end(), n) != fighters.end(); };
    return has(asked) ? asked : has("RYU") ? std::string("RYU") : fighters.empty() ? std::string() : fighters.front();
}

Reply Themes(const Request& r) {
    nlohmann::json names = nlohmann::json::array();
    for (const std::string& name : platform::game_themes::Names(r.host.directory(), platform::game_themes::BattleHud)) {
        std::error_code ignored;
        if (fs::is_regular_file(HudFolder(r.host, name) / "cockpit.ems.emz", ignored)) names.push_back(name);
    }
    return Say(200, { { "themes", names } });
}
Reply ThemeInfo(const Request& r) {
    Theme theme;
    if (!OpenTheme(r.host, Param(r.query, "name"), theme)) return Fail(404, "no such theme");
    nlohmann::json elements = nlohmann::json::array(), art = nlohmann::json::array();
    for (const layout::Info& i : layout::Describe(theme.files)) elements.push_back(InfoJson(i));
    for (const Art& a : ArtOf(r.host, theme.name)) art.push_back(ArtName(a));
    return Say(200, { { "name", theme.name }, { "elements", elements }, { "layout", layout::LayoutToJson(Saved(r.host, theme.name)) }, { "art", art } });
}
Reply Scene(const Request& r) {
    Theme theme;
    if (!OpenTheme(r.host, Param(r.query, "name"), theme)) return Fail(404, "no such theme");
    const std::string fighter = FighterOf(r.host, Param(r.query, "fighter"));
    nlohmann::json sprites = nlohmann::json::array(), sheets = nlohmann::json::object();
    for (const layout::Sprite& sp : layout::Sprites(theme.files)) {
        sprites.push_back({ { "n", sp.node }, { "e", sp.element }, { "f", sp.file }, { "t", sp.tex }, { "uv", { sp.uv[0], sp.uv[1], sp.uv[2], sp.uv[3] } },
            { "r", { sp.r[0], sp.r[1], sp.r[2], sp.r[3] } }, { "p", { sp.pos[0], sp.pos[1], sp.pos[2] } }, { "rot", sp.rot }, { "add", sp.additive } });
    }
    for (const std::string& file : layout::Files()) {
        std::vector<std::string> dds;
        if (!theme.files.count(file) || !Sheet(r.host, theme.name, file, fighter, dds)) continue;
        nlohmann::json sizes = nlohmann::json::array();
        for (const std::string& d : dds) { std::uint32_t h = 0, w = 0; if (d.size() >= 20) { std::memcpy(&h, d.data() + 12, 4); std::memcpy(&w, d.data() + 16, 4); } sizes.push_back({ w, h }); }
        sheets[file] = sizes;
    }
    return Say(200, { { "fighter", fighter }, { "fighters", Fighters(r.host) }, { "sprites", sprites }, { "sheets", sheets } });
}
Reply SheetPicture(const Request& r) {
    Theme theme;
    if (!OpenTheme(r.host, Param(r.query, "name"), theme)) return Fail(404, "no such theme");
    const std::string relative = SheetOf(Param(r.query, "file"), FighterOf(r.host, Param(r.query, "fighter")));
    std::string sheet; std::vector<std::string> dds; textures::Image image;
    const int index = std::atoi(Param(r.query, "index").c_str());
    // The user's pictures show in the preview, pasted as the game will read them.
    if (relative.empty() || !Read(Find(r.host, theme.name, relative), sheet) || !textures::Entries(WithArt(r.host, theme.name, relative, std::move(sheet)), dds)
        || index < 0 || static_cast<std::size_t>(index) >= dds.size() || !textures::Decode(dds[index], image)) return Fail(404, "no such texture");
    Reply reply; reply.status = 200; reply.type = "image/png"; reply.body = textures::EncodePng(image);
    return reply;
}
Reply Image(const Request& r) {
    const auto& json = r.json;
    Theme theme;
    if (!PostedTheme(r, theme)) return Fail(404, "no such theme");
    Art a;
    const auto uv = json.value("uv", nlohmann::json());
    a.file = json.value("file", std::string()); a.fighter = json.value("fighter", std::string()); a.tex = json.value("tex", -1);
    if (a.file != "chara" && a.file != "uc") a.fighter.clear();
    if (!uv.is_array() || uv.size() != 4) return Fail(400, "bad region");
    for (int i = 0; i < 4; ++i) a.u[i] = uv[i].is_number_integer() ? uv[i].get<int>() : -1;
    textures::Image picture;
    if (!Valid(a)) return Fail(400, "bad region");
    if (!textures::DecodePng(Unbase64(json.value("png", std::string())), picture) || picture.width != a.u[2] - a.u[0] || picture.height != a.u[3] - a.u[1]) return Fail(400, "bad picture");
    if (!WriteFile(ArtFolder(r.host, theme.name) / (ArtName(a) + ".png"), textures::EncodePng(picture))) return Fail(500, "cannot save");
    return Say(200, { { "ok", true }, { "name", ArtName(a) } });
}
Reply ImageRemove(const Request& r) {
    const auto& json = r.json;
    Theme theme; Art a;
    if (!PostedTheme(r, theme)) return Fail(404, "no such theme");
    const std::string stem = json.value("name", std::string());
    if (!ParseArt(stem, a)) return Fail(400, "bad name");
    std::error_code ignored; fs::remove(ArtFolder(r.host, theme.name) / (stem + ".png"), ignored);
    return Say(200, { { "ok", true } });
}
Reply Restart(const Request& r) { r.host.restart(); return Say(200, { { "ok", true } }); }
Reply SaveAs(const Request& r) {
    const Host& host = r.host;
    const auto& json = r.json;
    const std::string copy = Text(json, "name");
    Theme theme; std::map<std::string, std::string> written;
    if (!PostedTheme(r, theme)) return Fail(404, "no such theme");
    std::error_code failed;
    if (!PlainName(copy) || fs::exists(ThemeFolder(host, copy), failed)) return Fail(400, "name taken or not allowed");
    if (!Build(host, theme.name, theme.files, Saved(host, theme.name), written)) return Fail(500, "cannot read the theme");
    // The copy is the theme as the game reads it: the edits are in its files, so it keeps neither layout.json nor the pictures.
    fs::create_directories(ThemeFolder(host, copy), failed);
    for (fs::directory_iterator it(ThemeFolder(host, theme.name), failed), end; !failed && it != end; it.increment(failed)) {
        const std::string leaf = it->path().filename().string();
        if (leaf == "layout.json" || leaf == "layout-art" || leaf == "review.json") continue;
        fs::copy(it->path(), ThemeFolder(host, copy) / it->path().filename(), fs::copy_options::recursive, failed);
    }
    bool saved = !failed;
    for (const auto& [file, bytes] : written) saved = saved && WriteFile(ThemeFolder(host, copy) / "battle" / "hud" / fs::u8path(file), bytes);
    if (!saved) { fs::remove_all(ThemeFolder(host, copy), failed); return Fail(500, "cannot save"); }
    return Say(200, { { "ok", true }, { "name", copy } });
}
// "layout" saves the edits; "apply" saves them if sent, then writes the theme into the game.
Reply Edit(const Request& r, bool apply) {
    const auto& json = r.json;
    Theme theme;
    if (!PostedTheme(r, theme)) return Fail(404, "no such theme");
    const bool given = json.contains("layout");
    const layout::Layout edits = given ? layout::LayoutFromJson(json["layout"]) : Saved(r.host, theme.name);
    if (given && !Save(r.host, theme.name, edits)) return Fail(500, "cannot save");
    if (!apply) return Say(200, { { "ok", true } });
    std::map<std::string, std::string> written;
    if (!Build(r.host, theme.name, theme.files, edits, written)) return Fail(500, "cannot read the theme");
    r.host.apply(theme.name, std::move(written));
    return Say(200, { { "ok", true } });
}
Reply Layout(const Request& r) { return Edit(r, false); }
Reply Apply(const Request& r) { return Edit(r, true); }

struct Route { bool post; const char* name; Reply (*run)(const Request&); };
const Route Routes[] = {
    { false, "themes", Themes }, { false, "theme", ThemeInfo }, { false, "scene", Scene }, { false, "sheet", SheetPicture },
    { true, "image", Image }, { true, "image-remove", ImageRemove }, { true, "restart", Restart }, { true, "save-as", SaveAs },
    { true, "layout", Layout }, { true, "apply", Apply },
};
}

void Allow(bool allowed) { s_allowed = allowed; }
bool Allowed() { return s_allowed; }
bool Owns(const std::string& path) { return path.compare(0, 9, "/api/hud/") == 0; }

Reply Handle(const Host& host, bool post, const std::string& path, const std::string& query, const std::string& body, const std::string& userAgent) {
    if (!s_allowed) return Fail(403, "off");
    if (!layout::DesktopAgent(userAgent)) return Fail(403, "desktop");
    const std::string route = path.substr(9);
    for (const Route& candidate : Routes)
        if (candidate.post == post && route == candidate.name) return candidate.run({ host, query, post ? nlohmann::json::parse(body, nullptr, false) : nlohmann::json() });
    return Fail(404, "no");
}
} } } }
