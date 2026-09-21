#pragma once

// Headless ImGui for the UI tests that draw without a device: no ini file, a
// 1280x960 display and the product theme, which also builds the font atlas.
#include "../ui/Theme.hxx"
#include <imgui.h>

struct HeadlessImGui {
    ImGuiIO& io;
    HeadlessImGui() : io((ImGui::CreateContext(), ImGui::GetIO())) {
        io.IniFilename = nullptr; io.DisplaySize = ImVec2(1280, 960);
        sf4e::ui::ApplyTheme(1);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(); }
    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;
};
