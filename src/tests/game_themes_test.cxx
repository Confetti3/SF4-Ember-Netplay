#include "../platform/GameThemes.hxx"
#include "test_support.hxx"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;
using namespace sf4e::platform;

struct Fixture {
    fs::path root;
    Fixture() {
        wchar_t temp[MAX_PATH], unique[MAX_PATH];
        CHECK(GetTempPathW(MAX_PATH, temp));
        CHECK(GetTempFileNameW(temp, L"s4t", 0, unique));
        CHECK(DeleteFileW(unique));
        root = unique;
        CHECK(fs::create_directory(root));
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
    void File(const fs::path& name, const std::string& bytes = "#EMB") {
        const fs::path path = root / name;
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        CHECK(out.write(bytes.data(), bytes.size()));
    }
    fs::path Active() const { return root / game_themes::ActiveFolder / game_themes::MainMenu; }
    bool Menus(const fs::path& name) const { return fs::is_regular_file(Active() / name); }
    game_themes::Outcome Use(const std::string& name, const wchar_t* part = game_themes::MainMenu) {
        std::string error;
        const auto outcome = game_themes::Use(root.wstring(), part, name, error);
        CHECK((outcome == game_themes::Outcome::Used) == error.empty());
        return outcome;
    }
};

int main() {
    Fixture f;
    CHECK(game_themes::Names(f.root.wstring(), game_themes::MainMenu).empty());

    f.File("ember/themes/Neon/ui/main_menu/vfx/main_menu.emz");
    f.File("ember/themes/Neon/battle/hud/ENG/hud.emz");
    f.File("ember/themes/Blue/ui/main_menu/title.emz");
    f.File("ember/themes/NoMenus/battle/hud/ENG/hud.emz");
    f.File("ember/themes/Title/ui/advertise/title/title_ultra.emz");
    const auto names = game_themes::Names(f.root.wstring(), game_themes::MainMenu);
    CHECK(names.size() == 2 && names[0] == "Blue" && names[1] == "Neon");

    // Only the theme's main menu part is used, not its other parts.
    using game_themes::Outcome;
    CHECK(f.Use("Neon") == Outcome::Used);
    CHECK(f.Menus("vfx/main_menu.emz") && !fs::exists(f.root / game_themes::ActiveFolder / "battle"));

    // Another theme replaces the first one whole.
    CHECK(f.Use("Blue") == Outcome::Used);
    CHECK(f.Menus("title.emz") && !f.Menus("vfx/main_menu.emz"));
    CHECK(!fs::exists(f.Active().wstring() + L".next") && !fs::exists(f.Active().wstring() + L".old"));

    // A name that is not one folder, a theme that is gone, or one without
    // menus leaves the game's own menus, never the theme before.
    CHECK(f.Use("..") == Outcome::GameOwn);
    CHECK(!fs::exists(f.Active()));
    CHECK(f.Use("Blue") == Outcome::Used && f.Use("Missing") == Outcome::GameOwn && !fs::exists(f.Active()));
    CHECK(f.Use("Blue") == Outcome::Used && f.Use("NoMenus") == Outcome::GameOwn && !fs::exists(f.Active()));

    // A theme in use that cannot be moved aside (the game has a file in it
    // open) stays, and says so.
    CHECK(f.Use("Neon") == Outcome::Used);
    {
        const HANDLE open = CreateFileW((f.Active() / "vfx/main_menu.emz").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(open != INVALID_HANDLE_VALUE);
        CHECK(f.Use("Blue") == Outcome::Unchanged && f.Menus("vfx/main_menu.emz"));
        CloseHandle(open);
    }

    // The battle HUD is a part of its own: it sits beside the menus in use and
    // goes without touching them.
    const auto huds = game_themes::Names(f.root.wstring(), game_themes::BattleHud);
    CHECK(huds.size() == 2 && huds[0] == "Neon" && huds[1] == "NoMenus");
    const fs::path hud = f.root / game_themes::ActiveFolder / game_themes::BattleHud / "ENG/hud.emz";
    CHECK(f.Use("NoMenus", game_themes::BattleHud) == Outcome::Used && fs::is_regular_file(hud));
    CHECK(f.Menus("vfx/main_menu.emz"));
    CHECK(f.Use("", game_themes::BattleHud) == Outcome::Used && !fs::exists(hud));
    CHECK(f.Menus("vfx/main_menu.emz"));

    // Each menu screen is a part of its own: a title screen from another
    // theme sits beside the main menu in use.
    CHECK(game_themes::Names(f.root.wstring(), game_themes::Parts[1]) == std::vector<std::string>{"Title"});
    CHECK(f.Use("Title", game_themes::Parts[1]) == Outcome::Used);
    CHECK(fs::is_regular_file(f.root / game_themes::ActiveFolder / "ui/advertise/title/title_ultra.emz") && f.Menus("vfx/main_menu.emz"));

    // No name: the game's own menus, and the themes themselves are kept.
    CHECK(f.Use("") == Outcome::Used);
    CHECK(!fs::exists(f.Active()));
    CHECK(fs::is_regular_file(f.root / "ember/themes/Blue/ui/main_menu/title.emz"));

    // A file written over the copy in use (the HUD editor's patched files): replaced whole, only inside the part.
    std::string error;
    CHECK(game_themes::WriteActive(f.root.wstring(), game_themes::BattleHud, "CMN/cockpit.ems.emz", "one", error));
    CHECK(game_themes::WriteActive(f.root.wstring(), game_themes::BattleHud, "CMN/cockpit.ems.emz", "two", error));
    std::ifstream written(f.root / game_themes::ActiveFolder / game_themes::BattleHud / "CMN/cockpit.ems.emz", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
    CHECK(text == "two" && !fs::exists(f.root / game_themes::ActiveFolder / game_themes::BattleHud / "CMN/cockpit.ems.emz.tmp"));
    CHECK(!game_themes::WriteActive(f.root.wstring(), game_themes::BattleHud, "../../escape.txt", "x", error) && !error.empty());
    CHECK(!game_themes::WriteActive(f.root.wstring(), game_themes::BattleHud, "", "x", error));
    return 0;
}
