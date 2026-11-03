#include "../platform/GameThemes.hxx"
#include "test_support.hxx"
#include <filesystem>
#include <fstream>
#include <string>
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
    fs::path Active() const { return root / game_themes::ActiveFolder / game_themes::Menus; }
    bool Menus(const fs::path& name) const { return fs::is_regular_file(Active() / name); }
    bool Use(const std::string& name) { std::string error; const bool done = game_themes::Use(root.wstring(), game_themes::Menus, name, error); CHECK(done == error.empty()); return done; }
};

int main() {
    Fixture f;
    CHECK(game_themes::Names(f.root.wstring(), game_themes::Menus).empty());

    f.File("ember/themes/Neon/ui/main_menu/vfx/main_menu.emz");
    f.File("ember/themes/Neon/battle/hud/ENG/hud.emz");
    f.File("ember/themes/Blue/ui/main_menu/title.emz");
    f.File("ember/themes/NoMenus/battle/hud/ENG/hud.emz");
    const auto names = game_themes::Names(f.root.wstring(), game_themes::Menus);
    CHECK(names.size() == 2 && names[0] == "Blue" && names[1] == "Neon");

    // Only the theme's menus part is used: never anything a match reads.
    CHECK(f.Use("Neon"));
    CHECK(f.Menus("main_menu/vfx/main_menu.emz") && !fs::exists(f.root / game_themes::ActiveFolder / "battle"));

    // Another theme replaces the first one whole.
    CHECK(f.Use("Blue"));
    CHECK(f.Menus("main_menu/title.emz") && !f.Menus("main_menu/vfx/main_menu.emz"));
    CHECK(!fs::exists(f.Active().wstring() + L".next") && !fs::exists(f.Active().wstring() + L".old"));

    // A name that is not one folder, a theme that is gone, or one without
    // menus leaves the game's own menus, never the theme before.
    CHECK(!f.Use(".."));
    CHECK(!fs::exists(f.Active()));
    CHECK(f.Use("Blue") && !f.Use("Missing") && !fs::exists(f.Active()));
    CHECK(f.Use("Blue") && !f.Use("NoMenus") && !fs::exists(f.Active()));

    // A theme in use that cannot be moved aside (the game has a file in it
    // open) stays, and says so.
    CHECK(f.Use("Neon"));
    {
        const HANDLE open = CreateFileW((f.Active() / "main_menu/vfx/main_menu.emz").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(open != INVALID_HANDLE_VALUE);
        CHECK(!f.Use("Blue") && f.Menus("main_menu/vfx/main_menu.emz"));
        CloseHandle(open);
    }

    // No name: the game's own menus, and the themes themselves are kept.
    CHECK(f.Use(""));
    CHECK(!fs::exists(f.Active()));
    CHECK(fs::is_regular_file(f.root / "ember/themes/Blue/ui/main_menu/title.emz"));
    return 0;
}
