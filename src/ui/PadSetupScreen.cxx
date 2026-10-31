#include "PadSetupScreen.hxx"
#include "GameMenu.hxx"
#include <cmath>

namespace sf4e { namespace ui {
using namespace gamepads;
std::string SetupStepName(int step) { return loc::T(SetupSteps[step].nameKey); }
std::vector<MenuEntry> PadSetupRows(const SetupState& state) {
    std::vector<MenuEntry> rows;
    for (int i = 0; i < SetupStepCount; ++i) {
        auto row = Row("step-" + std::to_string(i), SetupStepName(i), state.mode == SetupMode::Summary ? loc::T("pad_setup.redo_detail") : "");
        row.value = state.recorded[i] ? RawInputName(*state.recorded[i]) : loc::T(state.skipped[i] ? "pad_setup.skipped" : "common.not_set");
        row.valueHighlighted = state.mode == SetupMode::Summary && state.recorded[i] &&
            std::find(state.held.begin(), state.held.end(), *state.recorded[i]) != state.held.end();
        row.detailText = DetailText::Name;
        rows.push_back(std::move(row));
    }
    if (state.mode == SetupMode::Summary) {
        rows.push_back(Row("save", loc::T("pad_setup.save"), loc::T("pad_setup.save_detail"), state.CanSave() && state.feedback != SetupFeedback::Reconnect));
        rows.push_back(Row("restart", loc::T("pad_setup.start_over"), loc::T("pad_setup.start_over_detail")));
        rows.push_back(ConfirmRow("cancel", loc::T("pad_setup.cancel"), loc::T("pad_setup.cancel_detail")));
    }
    return rows;
}
std::vector<LegendHint> PadSetupLegend(const SetupState& state) {
    // Escape is Back when its hint is clicked.
    std::vector<LegendHint> hints{{"Esc", loc::T("common.cancel"), MenuInput::Back}};
    if (state.step > 0) hints.push_back({"Bksp", loc::T("pad_setup.back_step")});
    if (!SetupSteps[state.step].required) hints.push_back({"Space", loc::T("pad_setup.skip")});
    if (state.step > RightStep) hints.push_back({"generic_stick_left", loc::T("pad_setup.back_step")});
    if (state.step > RightStep && !SetupSteps[state.step].required) hints.push_back({"generic_stick_right", loc::T("pad_setup.skip")});
    hints.push_back({"generic_button_circle_fill", loc::T("pad_setup.hold_to_cancel")});
    return hints;
}
void DrawPadSetupDetail(const PadView& pad, const SetupState& state) {
    const float s = Scale(), size = 64 * s;
    const auto image = MenuArt() ? MenuArt()->InputPrompt(pad.picture) : SelectionImage{};
    ImGui::BeginGroup();
    const auto at = ImGui::GetCursorScreenPos();
    if (image.texture) ImGui::GetWindowDrawList()->AddImage(image.texture, at, ImVec2(at.x + size, at.y + size), image.uvMin, image.uvMax);
    ImGui::Dummy(ImVec2(size, size));
    ImGui::SameLine(); ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    NoteUserText(pad.name.c_str()); ImGui::TextWrapped("%s", pad.name.c_str());
    ImGui::PopTextWrapPos(); ImGui::EndGroup(); ImGui::EndGroup();
    if (state.mode == SetupMode::Summary) {
        ImGui::TextWrapped("%s", loc::T("pad_setup.summary_detail"));
        if (state.feedback == SetupFeedback::Reconnect) ImGui::TextWrapped("%s", loc::Tf("pad_setup.reconnect", state.reconnectSeconds).c_str());
        return;
    }
    ImGui::TextUnformatted(loc::Tf("pad_setup.step_count", state.step + 1, SetupStepCount).c_str());
    const auto name = SetupStepName(state.step);
    const auto textAt = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), 1.4f * ImGui::GetFontSize(), textAt, palette::Ivory, name.c_str());
    ImGui::Dummy(ImVec2(0, 1.4f * ImGui::GetTextLineHeightWithSpacing()));
    const auto& step = SetupSteps[state.step];
    ImGui::TextWrapped("%s", loc::T(step.kind == StepKind::Button ? "pad_setup.press_button" : "pad_setup.press_direction"));
    if (!step.required) ImGui::TextWrapped("%s", loc::T(step.kind == StepKind::Stick ? "pad_setup.optional_stick" : "pad_setup.optional"));
    if (state.step == LkStep) ImGui::TextWrapped("%s", loc::T("pad_setup.lk_selects"));
    if (state.step == MkStep) ImGui::TextWrapped("%s", loc::T("pad_setup.mk_backs"));
    std::string feedback; Tone tone = Tone::Neutral;
    if (state.feedback == SetupFeedback::Reconnect) feedback = loc::Tf("pad_setup.reconnect", state.reconnectSeconds);
    else if (state.releaseButtons) feedback = loc::T("controller.release_buttons");
    else if (state.holdMs >= 1000) {
        const auto centre = ImGui::GetCursorScreenPos(); const float radius = 16 * s;
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddCircle(ImVec2(centre.x + radius, centre.y + radius), radius, palette::Muted, 40, 2 * s);
        draw->PathArcTo(ImVec2(centre.x + radius, centre.y + radius), radius, -1.570796f,
            -1.570796f + 6.283185f * (std::min)(1.f, (state.holdMs - 1000) / 2000.f), 40);
        draw->PathStroke(palette::Ember, 0, 3 * s);
        ImGui::Dummy(ImVec2(36 * s, 36 * s)); feedback = loc::T("pad_setup.hold_cancel");
    } else if (state.feedback == SetupFeedback::Conflict && state.conflictInput) {
        feedback = loc::Tf("pad_setup.conflict", RawInputName(*state.conflictInput), SetupStepName(state.conflictStep)); tone = Tone::Error;
    } else if (state.feedback == SetupFeedback::Required) { feedback = loc::T("pad_setup.required"); tone = Tone::Error; }
    else if (state.feedback == SetupFeedback::Idle) feedback = loc::T("pad_setup.idle_hint");
    if (!feedback.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(tone)); ImGui::TextWrapped("%s", feedback.c_str()); ImGui::PopStyleColor(); }
}
} }
