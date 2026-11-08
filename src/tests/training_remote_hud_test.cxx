// The remote's HUD layout routes: refused unless allowed and from a desktop,
// themes and elements listed, layout.json kept, and the patched files handed on.
#include "../platform/GameThemes.hxx"
#include "../platform/HudLayout.hxx"
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
    CHECK(appliedTo == "Test" && applied.count("cockpit") == 1);
    std::string ems;
    CHECK(Unpack(applied["cockpit"], ems) && hud_fixture::GetF(ems, 0x140) == 300 && hud_fixture::GetF(ems, 0x144) == 200);

    CHECK(post("/api/hud/apply", R"({"theme":"Test","layout":{}})").status == 200);   // nothing edited: the theme as it was
    CHECK(applied.empty());
    hud::Allow(false);
    std::error_code ignored; fs::remove_all(root, ignored);
    return 0;
}
