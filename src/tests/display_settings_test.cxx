#include "../platform/DisplaySettings.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../ui/DisplayPanel.hxx"
#include "../common/Localization.hxx"
#include "test_support.hxx"
#include "temp_root.hxx"
#include <fstream>

using namespace sf4e;
static ui::MenuAction Choose(const char* row, const char* value) {
    ui::MenuAction out; out.kind = ui::MenuAction::Chosen; out.id = row; out.text = value; return out;
}
static ui::MenuAction Activate(const char* row) {
    ui::MenuAction out; out.kind = ui::MenuAction::Activate; out.id = row; return out;
}
int main() {
    display::Monitor monitor;
    monitor.id = "DISPLAY1"; monitor.name = "Ultrawide"; monitor.primary = true;
    monitor.x = -3440; monitor.y = -100; monitor.width = 3440; monitor.height = 1440;
    monitor.workX = -3440; monitor.workY = -100; monitor.workWidth = 3440; monitor.workHeight = 1400;
    monitor.refresh = 144;
    monitor.modes = {{1920,1080,60}, {2560,1440,60}, {2560,1440,144}};
    display::Preferences preference;
    CHECK(preference.Valid() && display::Resolve(preference, monitor).mode == display::Mode::Native);
    preference.mode = display::Mode::Borderless;
    auto result = display::Resolve(preference, monitor);
    CHECK(result.width == 2560 && result.height == 1440 && result.window.x == -3000 && result.window.y == -100);
    preference.width = 1920; preference.height = 1080;
    result = display::Resolve(preference, monitor);
    CHECK(result.width == 1920 && result.window.width == 2560); // Proportional upscale.
    preference.mode = display::Mode::Windowed;
    result = display::Resolve(preference, monitor);
    CHECK(result.window.width <= monitor.workWidth && result.window.height <= monitor.workHeight);
    CHECK(result.window.x >= monitor.workX && result.window.y >= monitor.workY);
    CHECK(result.window.width * 9 == result.window.height * 16);
    preference = {}; preference.mode = display::Mode::Fullscreen;
    result = display::Resolve(preference, monitor);
    CHECK(result.mode == display::Mode::Fullscreen && result.width == 2560 && result.refresh == 144);
    preference.refresh = 60;
    CHECK(display::Resolve(preference, monitor).refresh == 60);
    preference.refresh = 120;
    result = display::Resolve(preference, monitor);
    CHECK(result.refresh == 144 && result.fallback);
    preference.width = 3840; preference.height = 2160;
    CHECK(display::Resolve(preference, monitor).mode == display::Mode::Borderless);
    std::vector<display::Monitor> monitors{monitor};
    CHECK(display::FindMonitor(monitors, "disconnected")->id == monitor.id);
    CHECK(!display::FindMonitor({}, ""));
    preference.monitor = "disconnected";
    CHECK(display::Resolve(preference, monitor).fallback);
    preference.width = 3440; preference.height = 1440;
    CHECK(!preference.Valid()); // Never offer stretching as proportional output.
    preference = {}; preference.mode = display::Mode::Borderless;
    const auto encoded = platform::EncodeDisplayPreferences(preference);
    display::Preferences decoded;
    CHECK(platform::DecodeDisplayPreferences(encoded, decoded) && decoded == preference);
    for (auto invalid : {nlohmann::json{{"version", 2}}, nlohmann::json::array(), nlohmann::json("invalid")})
        CHECK(!platform::DecodeDisplayPreferences(invalid, decoded));
    auto invalid = encoded; invalid["width"] = 18446744073709551615ull;
    CHECK(!platform::DecodeDisplayPreferences(invalid, decoded));
    invalid = encoded; invalid["mode"] = -1;
    CHECK(!platform::DecodeDisplayPreferences(invalid, decoded));
    invalid = encoded; invalid["refresh"] = "60";
    CHECK(!platform::DecodeDisplayPreferences(invalid, decoded));
    CHECK(platform::SetLaunchDisplayPreferences(preference));
    std::string error;
    CHECK(platform::LaunchDisplayPreferences(error) == preference);
    SetEnvironmentVariableA("SF4E_DISPLAY_CONFIG", "invalid");
    CHECK(platform::LaunchDisplayPreferences(error).mode == display::Mode::Native && !error.empty());
    SetEnvironmentVariableA("SF4E_DISPLAY_CONFIG", nullptr);

    const auto directory = MakeTempRoot(L"sf4e-display-test-");
    CHECK(std::filesystem::create_directory(directory));
    netplay::SettingsStore store(directory.wstring());
    CHECK(store.SaveLauncher({{"displayName", "Kept Player"}, {"inputDelay", 0}, {"autoInputDelay", false}}, error));
    CHECK(platform::SaveDisplayPreferences(preference, error, directory.wstring()));
    CHECK(platform::LoadDisplayPreferences(decoded, error, directory.wstring()) && decoded == preference);
    nlohmann::json settings;
    CHECK(store.LoadLauncher(settings, error));
    CHECK(settings["displayName"] == "Kept Player" && settings["inputDelay"] == 0 && settings["autoInputDelay"] == false);
    HANDLE lock = CreateFileW((directory / L"settings.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    CHECK(!platform::SaveDisplayPreferences({}, error, directory.wstring()));
    CloseHandle(lock);
    CHECK(platform::LoadDisplayPreferences(decoded, error, directory.wstring()) && decoded == preference);

    loc::SetActive(loc::Locale::En);
    bool saveFails = true;
    int saves = 0;
    ui::DisplayPanel panel(monitors, preference, [&](const display::Preferences& value, std::string& diagnostic) {
        ++saves; if (saveFails) { diagnostic = "busy"; return false; }
        return platform::SaveDisplayPreferences(value, diagnostic, directory.wstring());
    });
    panel.Handle(Choose("display-mode", "1"));
    CHECK(panel.Draft().mode == display::Mode::Windowed && panel.Saved() == preference);
    panel.Handle(Choose("display-resolution", "99999999x1"));
    CHECK(panel.Draft().width == 0);
    panel.Handle(Choose("display-resolution", "1920x1080"));
    CHECK(panel.Draft().width == 1920);
    panel.Handle(Activate("display-save"));
    CHECK(saves == 1 && !panel.Error().empty() && panel.Saved() == preference);
    saveFails = false;
    panel.Handle(Activate("display-save"));
    CHECK(saves == 2 && panel.Error().empty() && panel.Saved() == panel.Draft());
    panel.Handle(Choose("display-mode", "2")); panel.Discard();
    CHECK(panel.Draft().mode == display::Mode::Windowed);
    panel.Handle(Choose("display-monitor", "missing"));
    CHECK(panel.Draft().monitor.empty());
    panel.Handle(Activate("display-reset"));
    CHECK(panel.Saved().mode == display::Mode::Native);
    CHECK(platform::LoadDisplayPreferences(decoded, error, directory.wstring()) && decoded.mode == display::Mode::Native);
    CHECK(store.LoadLauncher(settings, error) && settings["inputDelay"] == 0 && settings["displayName"] == "Kept Player");
    RemoveTempRoot(directory);
    std::puts("Display selection, validation, persistence, save failures, choices and recovery passed.");
}
