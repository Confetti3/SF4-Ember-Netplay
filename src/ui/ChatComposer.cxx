#include "ChatComposer.hxx"
#include "GameMenu.hxx"
#include "../common/Localization.hxx"
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
namespace sf4e { namespace ui {
namespace {
// ImGui selects the whole box when the keyboard focus lands on it, which would
// have the next letter replace a kept draft. The caret goes to the end instead.
int CaretToEnd(ImGuiInputTextCallbackData* data) {
    bool* fresh = static_cast<bool*>(data->UserData);
    if (*fresh) { *fresh = false; data->CursorPos = data->BufTextLen; data->SelectionStart = data->SelectionEnd = data->CursorPos; }
    return 0;
}
bool HasText(const char* text) {
    for (; *text; ++text) if (!std::isspace(static_cast<unsigned char>(*text))) return true;
    return false;
}
}
void ChatComposer::ObserveScreen(GameMenu& menu) {
    if (screen_ != menu.navigation.Screen()) {
        Release(menu); screen_ = menu.navigation.Screen();
    }
}
void ChatComposer::Release(GameMenu& menu) {
    if (id_ && ImGui::GetActiveID() == id_) ImGui::ClearActiveID();
    typing_ = focus_ = false; id_ = 0; boxText_.clear();
    menu.navigation.NeutralGate();
}
ChatComposer::Result ChatComposer::Draw(GameMenu& menu, char (&draft)[257], std::uint64_t roomEpoch, float width, bool inlineBox, bool typable, bool pending, bool activate) {
    const float s = Scale(), gap = ImGui::GetStyle().ItemSpacing.x;
    const unsigned pressed = menu.TextPressed();
    const bool wasTyping = Typing();
    if (activate) focus_ = true;
    // Full width epoch, including its upper bits; a new room has a new widget.
    const auto epoch = std::to_string(roomEpoch);
    ImGui::PushID(epoch.c_str());
    const auto id = ImGui::GetID("##chat-draft");
    id_ = id;
    char buffer[sizeof(draft)]; std::memcpy(buffer, draft, sizeof(buffer));
    if (boxText_ != draft)
        if (auto* state = ImGui::GetInputTextState(id)) state->ReloadUserBufAndMoveToEnd();
    bool fresh = typable && (focus_ || (!inlineBox && !ImGui::IsAnyItemActive()));
    const float sendWidth = inlineBox ? ImGui::CalcTextSize(loc::T("chat.send")).x + 2*ImGui::GetStyle().FramePadding.x + 8*s : 0;
    ImGui::BeginDisabled(!typable);
    if (fresh) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth((std::max)(1.f, width - (inlineBox ? sendWidth + gap : 0)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4*s);
    const bool edited = ImGui::InputTextWithHint("##chat-draft", loc::T("chat.hint"), buffer, sizeof(buffer),
        ImGuiInputTextFlags_CallbackAlways, CaretToEnd, &fresh);
    ImGui::PopStyleVar();
    const bool active = ImGui::IsItemActive();
    focus_ = fresh && !active;
    const auto boxMin = ImGui::GetItemRectMin(), boxMax = ImGui::GetItemRectMax();
    ReportMenuCard(inlineBox ? "inline-chat" : "chat-box", boxMin, boxMax);
    if (active || (inlineBox && menu.navigation.Focus() == "inline-chat"))
        ImGui::GetWindowDrawList()->AddRect(boxMin, boxMax, palette::Ember, 4*s, 0, 2*s);
    if (inlineBox && active) menu.navigation.Prefer("inline-chat");
    const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (wasTyping && (pressed & MenuInput::Back));
    if (edited && !escape) std::snprintf(draft, sizeof(draft), "%s", buffer);
    boxText_ = draft;
    NoteUserText(draft, UserTextRole::Draft);
    typing_ = active && typable;
    bool clicked = false;
    if (inlineBox) {
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(!HasText(draft) || pending);
        clicked = ImGui::Button(loc::T("chat.send"), ImVec2(sendWidth, 0));
        ReportMenuCard("inline-chat-send", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::EndDisabled();
    }
    ImGui::EndDisabled(); ImGui::PopID();
    if (escape && wasTyping) {
        Release(menu);
        return Result::Released;
    }
    // Enter can release ImGui's active ID; ownership at frame start decides
    // whether that press belongs to this editor.
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    const bool send = wasTyping && (active || enter) && !activate && (pressed & MenuInput::Select);
    if (typable && send) focus_ = true;
    if (typable && HasText(draft) && !pending && (clicked || send)) {
        focus_ = true;
        return Result::Sent;
    }
    return Result::None;
}

} }
