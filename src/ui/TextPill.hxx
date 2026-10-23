#pragma once
#include "Theme.hxx"
#include <imgui.h>

namespace sf4e { namespace ui {
inline void DrawButtonPill(ImDrawList* draw, const char* text, ImVec2 at, float width, float height,
    float unit, float alpha = 1, bool active = false) {
    draw->AddRect(ImVec2(at.x + .5f, at.y + .5f), ImVec2(at.x + width - .5f, at.y + height - .5f),
        active ? palette::Ember : AccentColor(IM_COL32(243, 235, 221, static_cast<int>(130 * alpha))), height / 2, 0, (std::max)(1.f, unit));
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + 6 * unit, at.y),
        AccentColor(IM_COL32(243, 235, 221, static_cast<int>(215 * alpha))), text);
}
} }
