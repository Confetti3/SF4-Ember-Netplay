// The remote's HUD layout routes: refused unless allowed and from a desktop,
// themes and elements listed, layout.json kept, and the patched files handed on.
#include "../platform/GameThemes.hxx"
#include "../platform/HudLayout.hxx"
#include "../platform/HudTextures.hxx"
#include "../training/RemoteHud.hxx"
#include "../training/RemoteHudPage.hxx"
#include "hud_fixture.hxx"
#include "test_support.hxx"
#include <filesystem>
#include <fstream>
#include <windows.h>

namespace fs = std::filesystem;
using namespace sf4e::training::remote;
using sf4e::platform::hud_layout::Unpack;

namespace {
std::string Base64(const std::string& in) {
    static const char* const table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const unsigned v = (unsigned char)in[i] << 16 | (i + 1 < in.size() ? (unsigned char)in[i + 1] << 8 : 0) | (i + 2 < in.size() ? (unsigned char)in[i + 2] : 0);
        out += table[v >> 18 & 63]; out += table[v >> 12 & 63]; out += i + 1 < in.size() ? table[v >> 6 & 63] : '='; out += i + 2 < in.size() ? table[v & 63] : '=';
    }
    return out;
}
const char* const Desktop = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120";
const char* const Phone = "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) Mobile/15E148";
}

int main() {
    // The page the phone gets has the Layout tab spliced in, ahead of the call that starts it.
    const std::string page = PageWithHudEditor();
    const auto tab = page.find("tabs.push([\"layout\"");
    CHECK(page.find("id=\"t-layout\"") != std::string::npos && tab != std::string::npos);
    CHECK(tab < page.rfind("\nstart();") && page.find("id=\"t-layout\"") < page.find("</main>"));
    CHECK(page.find("Mobi") != std::string::npos);   // the script keeps phones out

    wchar_t temp[MAX_PATH], unique[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, temp));
    CHECK(GetTempFileNameW(temp, L"hud", 0, unique));
    CHECK(DeleteFileW(unique));
    const fs::path root = unique;
    const fs::path hudFolder = root / sf4e::platform::game_themes::Folder / "Test" / "battle" / "hud" / "CMN";
    CHECK(fs::create_directories(hudFolder));
    {
        const std::string emz = sf4e::platform::hud_layout::Pack(hud_fixture::OneNode());
        std::ofstream(hudFolder / "cockpit.ems.emz", std::ios::binary).write(emz.data(), static_cast<std::streamsize>(emz.size()));
    }
    {
        const std::string tex = sf4e::platform::hud_layout::Pack(hud_fixture::OneSheet());
        std::ofstream(hudFolder / "cockpit.tex.emz", std::ios::binary).write(tex.data(), static_cast<std::streamsize>(tex.size()));
    }
    fs::create_directories(root / "resource" / "battle" / "hud" / "ENG" / "RYU");
    fs::create_directories(root / sf4e::platform::game_themes::Folder / "NoHud" / "battle" / "hud");

    std::string appliedTo; std::map<std::string, std::string> applied;
    const hud::Host host{ [&] { return root.wstring(); }, [&](const std::string& name, std::map<std::string, std::string> files) { appliedTo = name; applied = std::move(files); } };
    const auto get = [&](const std::string& path, const std::string& query = "", const char* agent = Desktop) { return hud::Handle(host, false, path, query, "", agent); };
    const auto post = [&](const std::string& path, const std::string& body, const char* agent = Desktop) { return hud::Handle(host, true, path, "", body, agent); };

    hud::Allow(false);
    CHECK(get("/api/hud/themes").status == 403);               // not allowed by the player
    hud::Allow(true);
    CHECK(get("/api/hud/themes", "", Phone).status == 403);    // a phone never edits
    CHECK(post("/api/hud/apply", "{}", Phone).status == 403);

    auto themes = nlohmann::json::parse(get("/api/hud/themes").body);
    CHECK(themes["themes"].size() == 1 && themes["themes"][0] == "Test");   // NoHud has no cockpit file

    CHECK(get("/api/hud/theme", "name=..").status == 404);
    CHECK(get("/api/hud/theme", "name=Missing").status == 404);
    auto theme = nlohmann::json::parse(get("/api/hud/theme", "name=Test").body);
    CHECK(theme["elements"].size() == 2 && theme["layout"].empty());

    // The sprites, and the textures they cut from.
    CHECK(get("/api/hud/scene", "name=Missing").status == 404);
    const auto scene = nlohmann::json::parse(get("/api/hud/scene", "name=Test&fighter=RYU").body);
    CHECK(scene["fighter"] == "RYU" && scene["sprites"].size() == 1);
    CHECK(scene["sprites"][0]["n"] == "1_vital_base_2" && scene["sprites"][0]["e"] == "p1_plate" && scene["sprites"][0]["uv"][2] == 4);
    CHECK(scene["sheets"]["cockpit"].size() == 1 && scene["sheets"]["cockpit"][0][0] == 4);
    const auto sheet = get("/api/hud/sheet", "name=Test&file=cockpit&index=0");
    CHECK(sheet.status == 200 && sheet.type == "image/png" && sheet.body.compare(1, 3, "PNG") == 0);
    CHECK(get("/api/hud/sheet", "name=Test&file=cockpit&index=3").status == 404);
    CHECK(get("/api/hud/sheet", "name=Test&file=../x&index=0").status == 404);

    CHECK(post("/api/hud/layout", R"({"theme":"../x","layout":{}})").status == 404);
    CHECK(post("/api/hud/layout", R"({"theme":"Test","layout":{"p1_plate":{"x":300,"y":200,"rot":0,"s":1,"hide":false}}})").status == 200);
    CHECK(fs::exists(root / sf4e::platform::game_themes::Folder / "Test" / "layout.json"));
    theme = nlohmann::json::parse(get("/api/hud/theme", "name=Test").body);
    CHECK(theme["layout"]["p1_plate"]["x"] == 300);

    CHECK(post("/api/hud/apply", R"({"theme":"Test"})").status == 200);   // the saved layout
    CHECK(appliedTo == "Test" && applied.count("CMN/cockpit.ems.emz") == 1);
    std::string ems;
    CHECK(Unpack(applied["CMN/cockpit.ems.emz"], ems) && hud_fixture::GetF(ems, 0x140) == 300 && hud_fixture::GetF(ems, 0x144) == 200);

    CHECK(post("/api/hud/apply", R"({"theme":"Test","layout":{}})").status == 200);   // nothing edited: the theme as it was
    CHECK(applied.empty());

    // A picture over part of the sheet: kept beside the theme, shown in the preview, and put in the sheet when applied.
    sf4e::platform::hud_textures::Image picture; picture.width = 2; picture.height = 2; picture.rgba.assign(16, 0);
    for (int i = 0; i < 4; ++i) { picture.rgba[4 * i] = 1; picture.rgba[4 * i + 1] = 2; picture.rgba[4 * i + 2] = 3; picture.rgba[4 * i + 3] = 250; }
    const std::string png = Base64(sf4e::platform::hud_textures::EncodePng(picture));
    CHECK(post("/api/hud/image", R"({"theme":"Test","file":"cockpit","tex":0,"uv":[1,1,3,3],"png":")" + png + "\"}").status == 200);
    CHECK(post("/api/hud/image", R"({"theme":"Test","file":"cockpit","tex":0,"uv":[1,1,4,4],"png":")" + png + "\"}").status == 400);   // not the region's size
    CHECK(post("/api/hud/image", R"({"theme":"Test","file":"../x","tex":0,"uv":[1,1,3,3],"png":")" + png + "\"}").status == 400);
    CHECK(fs::exists(root / sf4e::platform::game_themes::Folder / "Test" / "layout-art" / "cockpit-0-1-1-3-3.png"));
    theme = nlohmann::json::parse(get("/api/hud/theme", "name=Test").body);
    CHECK(theme["art"].size() == 1 && theme["art"][0] == "cockpit-0-1-1-3-3");
    {
        const auto view = get("/api/hud/sheet", "name=Test&file=cockpit&index=0");
        sf4e::platform::hud_textures::Image seen;
        CHECK(view.status == 200 && sf4e::platform::hud_textures::DecodePng(view.body, seen) && seen.rgba[(1 * 4 + 1) * 4] == 1 && seen.rgba[0] == 255);
    }
    CHECK(post("/api/hud/apply", R"({"theme":"Test","layout":{}})").status == 200);
    CHECK(applied.size() == 1 && applied.count("CMN/cockpit.tex.emz") == 1);
    {
        std::vector<std::string> dds; sf4e::platform::hud_textures::Image sheet;
        CHECK(sf4e::platform::hud_textures::Entries(applied["CMN/cockpit.tex.emz"], dds) && sf4e::platform::hud_textures::Decode(dds[0], sheet) && sheet.rgba[(2 * 4 + 2) * 4 + 2] == 3 && sheet.rgba[0] == 255);
    }
    CHECK(post("/api/hud/image-remove", R"({"theme":"Test","name":"cockpit-0-1-1-3-3"})").status == 200);
    CHECK(!fs::exists(root / sf4e::platform::game_themes::Folder / "Test" / "layout-art" / "cockpit-0-1-1-3-3.png"));
    hud::Allow(false);
    std::error_code ignored; fs::remove_all(root, ignored);
    return 0;
}
