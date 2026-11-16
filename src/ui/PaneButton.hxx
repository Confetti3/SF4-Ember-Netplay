#pragma once
// The big button of a row's detail pane: the row's primary choice (PrimaryChoice) taken with a
// press, no chooser opened. Replays and the Archive tab draw it the same way.
#include "MenuNavigation.hxx"
#include "MenuProbes.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include <imgui.h>

namespace sf4e { namespace ui {

// A Primary board button across the pane at the cursor, labelled with the choice's own words.
// When the choice is not available the button is drawn disabled and its reason is said under it,
// so a press that cannot work is never silent. The press reaches the shell as the Chosen action
// a picked choice gives (MenuNavigation::Press).
inline void DrawPrimaryChoice(MenuNavigation& nav, const MenuEntry& row, const std::vector<MenuEntry>& rows) {
    const MenuChoice* choice = PrimaryChoice(row);
    if (!choice) return;
    const float s = Scale();
    ImGui::Dummy(ImVec2(0, 6 * s));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const v2::Box box{at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + 48 * s)};
    const bool pressed = ImGui::InvisibleButton("##primary-choice", ImVec2(box.W(), box.H()));
    const bool hovered = ImGui::IsItemHovered();
    v2::Button(ImGui::GetWindowDrawList(), box, choice->label, v2::ButtonKind::Primary, false, hovered, choice->enabled);
    if (MenuProbesActive()) ReportMenuCard("pane-choice", box.min, box.max);
    if (pressed && choice->enabled) nav.Press(row.id, choice->id, rows);
    if (choice->enabled) return;
    ImGui::Dummy(ImVec2(0, 4 * s));
    v2::Paragraph(choice->detail, 14 * s, v2::Colors().muted);
    ReportMenuCardText("pane-choice", choice->detail);
}

} }
