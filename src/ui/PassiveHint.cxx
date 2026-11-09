#include "PassiveHint.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
void DrawPassiveHint(const char* name, const char* text, float top, float alpha) {
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    const auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .5f, vp->Pos.y + top * Scale()), ImGuiCond_Always, ImVec2(.5f, 0));
    ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(text); ImGui::End();
    ImGui::PopStyleVar();
}

void DrawShortcutHint(float alpha) {
    if (alpha > 0) DrawPassiveHint("Ember shortcut", loc::T("runtime.open_shortcut"), 12, alpha);
}
} }
