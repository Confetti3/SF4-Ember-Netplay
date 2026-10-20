#include "../common/UiAccent.hxx"
#include "../netplay/UiAccentPreference.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/Theme.hxx"
#include "test_support.hxx"
#include "temp_root.hxx"
#include <limits>
#include <fstream>

int main() {
    using namespace sf4e;
    using Json = nlohmann::json;
    unsigned color = accent::Default;
    CHECK(accent::Parse("#1267aB", color) && color == 0x1267AB);
    CHECK(accent::Hex(color) == "#1267AB");
    CHECK(accent::Parse("000000", color) && color == 0);
    CHECK(accent::Parse("FFFFFF", color) && color == accent::Maximum);
    for (const char* invalid : {"", "#", "#123", "#1234567", " 123456", "12xx56", "0x123456"}) {
        color = 0x123456;
        CHECK(!accent::Parse(invalid, color) && color == 0x123456);
    }
    CHECK(accent::Adjust(0xFF1234, 16, 1) == 0xFF1234);
    CHECK(accent::Adjust(0x120034, 8, -1) == 0x120034);
    CHECK(accent::Adjust(0x123456, 0, 1) == 0x123457);
    CHECK(netplay::ReadUiAccent(Json::object()) == accent::Default);
    for (const Json invalid : {Json(-1), Json(0x1000000), Json(true), Json(1.5), Json("#FFFFFF"),
        Json((std::numeric_limits<std::uint64_t>::max)()), Json(nullptr)}) {
        CHECK(netplay::ReadUiAccent(Json{{"uiAccent", invalid}}) == accent::Default);
    }
    const auto folder = MakeTempRoot(L"sf4e-accent-");
    netplay::SettingsStore store(folder.wstring());
    std::string error;
    Json saved;
    CHECK(store.SaveLauncher(Json{{"uiAccent", 0x1267AB}, {"displayName", "Accent test"}}, error));
    CHECK(store.SaveOverlay(Json{{"unrelated", true}}, error));
    CHECK(store.LoadLauncher(saved, error) && netplay::ReadUiAccent(saved) == 0x1267AB);
    {
        std::ifstream file(folder / L"settings.json");
        const Json document = Json::parse(file);
        CHECK(document["netplay"]["uiAccent"] == 0x1267AB);
    }
    CHECK(store.SaveLauncher(Json{{"uiAccent", accent::Default}}, error));
    CHECK(store.LoadLauncher(saved, error) && netplay::ReadUiAccent(saved) == accent::Default);
    CHECK(saved["displayName"] == "Accent test");
    RemoveTempRoot(folder);

    netplay::PlayerPreferences original, changed;
    changed.uiAccent = 0x1267AB;
    CHECK(changed.Valid() && !ui::SamePreferences(original, changed));
    changed.uiAccent = 0x1000000;
    CHECK(!changed.Valid());

    auto* context = ImGui::CreateContext();
    const auto* atlas = ImGui::GetIO().Fonts;
    const auto fontCount = atlas->Fonts.Size;
    const auto success = ui::ToneColor(ui::Tone::Success);
    ui::SetAccentColor(0x1267AB);
    CHECK(ui::palette::Ember == IM_COL32(18, 103, 171, 255));
    CHECK(ui::AccentAlpha(60) == IM_COL32(18, 103, 171, 60));
    CHECK(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_NavCursor]) == ui::palette::Ember);
    CHECK(ImGui::GetIO().Fonts == atlas && atlas->Fonts.Size == fontCount);
    CHECK(ImGui::ColorConvertFloat4ToU32(ui::ToneColor(ui::Tone::Success)) == ImGui::ColorConvertFloat4ToU32(success));
    ui::SetAccentColor(accent::Default);
    CHECK(ui::palette::Ember == IM_COL32(255, 135, 56, 255));
    CHECK(ImGui::ColorConvertFloat4ToU32(ui::AccentShade(0x59402B)) == IM_COL32(89, 64, 43, 255));
    ImGui::DestroyContext(context);
}
