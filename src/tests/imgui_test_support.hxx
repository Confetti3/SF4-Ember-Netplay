#pragma once

// Headless ImGui for the UI tests that draw without a device: no ini file, a
// 1280x960 display and the product theme, which also builds the font atlas.
#include "../ui/Theme.hxx"
#include <imgui.h>

// Each one owns its own context and destroys that one, so several can be alive
// at once and go in any order; Use() makes this one current again.
struct HeadlessImGui {
    ImGuiContext* const context;
    ImGuiIO& io;
    HeadlessImGui() : context(ImGui::CreateContext()), io(ImGui::GetIO()) {
        io.IniFilename = nullptr; io.DisplaySize = ImVec2(1280, 960);
        sf4e::ui::ApplyTheme(1);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context); }
    void Use() { ImGui::SetCurrentContext(context); }
    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;
};
