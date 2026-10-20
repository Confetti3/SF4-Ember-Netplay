#include "../common/UiAccent.hxx"
#include "../netplay/UiAccentPreference.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/Theme.hxx"
#include "../ui/RoomCards.hxx"
#include "imgui_test_support.hxx"
#include "test_support.hxx"
#include "temp_root.hxx"
#include <cmath>
#include <limits>
#include <fstream>

namespace {
bool Same(ImVec4 a, ImVec4 b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
float Brightness(ImVec4 color) { return (std::max)(color.x, (std::max)(color.y, color.z)); }
float Luminance(ImVec4 color) {
    const auto linear = [](float c) { return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f); };
    return .2126f * linear(color.x) + .7152f * linear(color.y) + .0722f * linear(color.z);
}
}

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

    HeadlessImGui imgui;
    const auto* atlas = ImGui::GetIO().Fonts;
    const auto fontCount = atlas->Fonts.Size;
    const auto defaultStyle = ImGui::GetStyle();
    const ImVec4 semantic[] = {ui::ToneColor(ui::Tone::Success), ui::ToneColor(ui::Tone::Pending), ui::ToneColor(ui::Tone::Error)};
    const ImVec4 focused(.40f,.23f,.12f,.9f), pale(1,.82f,.65f,.7f);
    ui::SetAccentColor(0x1267AB);
    CHECK(ui::palette::Ember == IM_COL32(18, 103, 171, 255));
    CHECK(ui::AccentAlpha(60) == IM_COL32(18, 103, 171, 60));
    CHECK(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_NavCursor]) == ui::palette::Ember);
    CHECK(ImGui::GetIO().Fonts == atlas && atlas->Fonts.Size == fontCount);
    // A blue theme changes whole cards and text, not only their thin focus line.
    const auto panel = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const auto text = ImGui::GetStyle().Colors[ImGuiCol_Text];
    CHECK(panel.z > panel.x && text.z > text.x);
    CHECK(ui::palette::Ivory != IM_COL32(243,235,221,255) && ui::palette::Muted != IM_COL32(181,169,155,255));
    CHECK(ui::AccentShade(pale).z > ui::AccentShade(pale).x);
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(800,400));
    ImGui::Begin("Palette card");
    ui::DrawCardButton("sample",ImVec2(600,160),true,false,false);
    const auto expectedFill = ImGui::ColorConvertFloat4ToU32(ui::AccentShade(focused));
    bool foundFill = false;
    for (const auto& vertex : ImGui::GetWindowDrawList()->VtxBuffer)
        if (vertex.col == expectedFill) foundFill = true;
    ImGui::End();ImGui::Render();
    CHECK(foundFill);
    for (const unsigned chosen : {0x328DFFu, 0x004080u, 0xFFFF00u, 0xFFFFFFu, 0u}) {
        ui::SetAccentColor(chosen);
        for (const auto original : {focused, pale, ImVec4(.08f,.075f,.07f,.6f)}) {
            const auto mapped = ui::AccentShade(original);
            CHECK(std::abs(Brightness(mapped) - Brightness(original)) < .00001f && mapped.w == original.w);
        }
        const auto foreground = ImGui::GetStyle().Colors[ImGuiCol_Text];
        const auto background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        CHECK((Luminance(foreground)+.05f)/(Luminance(background)+.05f) > 7.f);
        for (unsigned alpha = 0; alpha <= 255; ++alpha)
            CHECK((ui::AccentColor(IM_COL32(74,46,20,alpha)) & IM_COL32_A_MASK) == (alpha << IM_COL32_A_SHIFT));
        CHECK(Same(ui::ToneColor(ui::Tone::Success), semantic[0]));
        CHECK(Same(ui::ToneColor(ui::Tone::Pending), semantic[1]));
        CHECK(Same(ui::ToneColor(ui::Tone::Error), semantic[2]));
        CHECK(ui::palette::Ready == IM_COL32(164,206,160,255));
    }
    ui::SetAccentColor(accent::Default);
    CHECK(ui::palette::Ember == IM_COL32(255, 135, 56, 255));
    CHECK(ui::palette::Ivory == IM_COL32(243,235,221,255) && ui::palette::Muted == IM_COL32(181,169,155,255));
    CHECK(Same(ui::AccentShade(focused), focused) && Same(ui::AccentShade(pale), pale));
    CHECK(ui::AccentColor(IM_COL32(74,46,20,235)) == IM_COL32(74,46,20,235));
    CHECK(ImGui::ColorConvertFloat4ToU32(ui::AccentShade(0x59402B)) == IM_COL32(89, 64, 43, 255));
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        CHECK(Same(ImGui::GetStyle().Colors[i], defaultStyle.Colors[i]));
    CHECK(ImGui::GetIO().Fonts == atlas && atlas->Fonts.Size == fontCount);
}
