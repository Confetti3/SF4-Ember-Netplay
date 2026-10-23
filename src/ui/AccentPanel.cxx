#include "AccentPanel.hxx"
#include "MenuRows.hxx"
#include "MenuProbes.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
std::vector<MenuEntry> AccentRows(unsigned rgb, bool enabled) {
    std::vector<MenuEntry> rows={
        Value("accent-red",loc::T("settings.accent_red"),std::to_string(accent::Channel(rgb,16)),"",enabled),
        Value("accent-green",loc::T("settings.accent_green"),std::to_string(accent::Channel(rgb,8)),"",enabled),
        Value("accent-blue",loc::T("settings.accent_blue"),std::to_string(accent::Channel(rgb,0)),"",enabled),
        Row("accent-hex",loc::T("settings.accent_hex"),loc::T("settings.accent_hex_detail"),enabled),
        Row("accent-reset",loc::T("settings.accent_reset"),"",enabled)};
    rows[3].text=true;rows[3].value=accent::Hex(rgb);rows[3].textLimit=7;
    return rows;
}
bool DrawAccentPicker(unsigned& rgb, bool enabled) {
    ImGui::TextUnformatted(loc::T("settings.accent_picker"));
    float channels[]={accent::Channel(rgb,16)/255.f,accent::Channel(rgb,8)/255.f,accent::Channel(rgb,0)/255.f};
    const auto space=ImGui::GetContentRegionAvail();
    const float width=(std::max)(70*Scale(),(std::min)(300*Scale(),(std::min)(space.x,space.y-12*Scale())));
    ImGui::SetNextItemWidth(width);
    ImGui::BeginDisabled(!enabled);
    const bool changed=ImGui::ColorPicker3("##accent-picker",channels,
        ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoSidePreview|ImGuiColorEditFlags_NoOptions|
        ImGuiColorEditFlags_NoAlpha|ImGuiColorEditFlags_PickerHueBar);
    ReportMenuCard("accent-picker",ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
    ImGui::EndDisabled();
    if(!enabled||!changed)return false;
    const auto byte=[](float value){return static_cast<unsigned>((std::max)(0.f,(std::min)(1.f,value))*255.f+.5f);};
    const unsigned picked=(byte(channels[0])<<16)|(byte(channels[1])<<8)|byte(channels[2]);
    if(picked==rgb)return false;
    rgb=picked;return true;
}
} }
