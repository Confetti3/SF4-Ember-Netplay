#include "ComboBlocks.hxx"
#include "ComboGlyphs.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include "../training/ComboBook.hxx"
#include <algorithm>
#include <cstdlib>

namespace sf4e { namespace ui {
namespace {
constexpr int DefaultGap = 20;
std::size_t focusedBlock = 0;
// A block's frames: the move's directions before its press, then the press.
int BlockFrames(const combo::Step& step) {
    int frames = 3;
    if (step.motion == "360" || step.motion == "720") return frames + (step.motion == "720" ? 32 : 16);
    for (std::size_t i = 0; i < step.motion.size(); ++i) frames += step.charge && i == 0 ? 50 : step.cancel ? 2 : 3;
    return frames;
}
bool Parse(const std::string& text, combo::Step& step) { std::string error; return combo::ParseStep(text, step, error); }
}
std::vector<MenuEntry> PatternRows(const std::vector<std::string>& steps) {
    std::vector<MenuEntry> rows;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        combo::Step step; Parse(steps[i], step);
        rows.push_back(Value("blk-" + std::to_string(i), std::to_string(i + 1) + ". " + steps[i], loc::Tf("training.combo.blocks.frame", (std::max)(0, step.at)), loc::T("training.combo.blocks.hint")));
    }
    rows.push_back(TextRow("blk-add", loc::T("training.combo.blocks.add"), "", 96));
    rows.push_back(ConfirmRow("blk-delete", loc::T("training.combo.blocks.delete"), loc::T("training.combo.blocks.delete.detail"), !steps.empty()));
    rows.push_back(Row("blk-replay", loc::T("training.combo.replay"), loc::T("training.combo.blocks.replay.detail"), !steps.empty()));
    return rows;
}
bool LayOutPattern(std::vector<std::string>& steps) {
    bool changed = false; int last = -DefaultGap;
    for (auto& text : steps) {
        combo::Step step;
        if (!Parse(text, step)) continue;
        if (step.at < 0) { step.at = (std::min)(combo::MaxAtFrame, last + DefaultGap); text = combo::Canonical(step); changed = true; }
        last = step.at;
    }
    return changed;
}
bool NudgePattern(std::vector<std::string>& steps, std::size_t index, int delta) {
    combo::Step step;
    if (index >= steps.size() || !Parse(steps[index], step)) return false;
    const int at = (std::max)(0, (std::min)(combo::MaxAtFrame, (std::max)(0, step.at) + delta));
    if (at == step.at) return false;
    step.at = at; steps[index] = combo::Canonical(step);
    return true;
}
bool AddToPattern(std::vector<std::string>& steps, const std::string& line, const std::string& fighter, std::string& error) {
    std::vector<std::string> added;
    if (!combo::ParseSteps(line, fighter, added, error)) return false;
    if (added.empty()) { error = combo::Clean(line); return false; }
    // A first move that is a cancel has nothing to cancel, so it links.
    if (steps.empty() && added[0].compare(0, 3, "xx ") == 0) added[0] = added[0].substr(3);
    if (steps.empty() && added[0].compare(0, 2, "~ ") == 0) added[0] = added[0].substr(2);
    steps.insert(steps.end(), added.begin(), added.end());
    LayOutPattern(steps);
    return true;
}
std::size_t FocusedPatternBlock() { return focusedBlock; }
bool DrawPattern(std::vector<std::string>& steps, const std::vector<MenuEntry>& entries, MenuNavigation& nav, float height, float unit) {
    bool changed = LayOutPattern(steps);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextDisabled("%s", loc::T("training.combo.blocks.guide"));
    ImGui::PopTextWrapPos();
    const auto& focus = nav.Focus();
    if (focus.compare(0, 4, "blk-") == 0 && focus != "blk-add" && focus != "blk-delete" && focus != "blk-replay")
        focusedBlock = static_cast<std::size_t>(std::atoi(focus.c_str() + 4));
    const float line = ImGui::GetTextLineHeight(), px = 3 * unit, blockHeight = line * 1.8f, ruler = line * 1.2f;
    std::vector<combo::Step> parsed(steps.size());
    int span = 60;
    for (std::size_t i = 0; i < steps.size(); ++i) if (Parse(steps[i], parsed[i])) span = (std::max)(span, parsed[i].at + BlockFrames(parsed[i]) + 30);
    // The lane scrolls sideways; the focused block is kept in view.
    const float laneHeight = ruler + blockHeight + line + 8 * unit;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.06f, .055f, .05f, .6f));
    ImGui::BeginChild("Pattern lane", ImVec2(0, laneHeight + ImGui::GetStyle().ScrollbarSize), 0, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const float width = span * px;
    for (int frame = 0; frame <= span; frame += 10) {
        const float x = origin.x + frame * px;
        const bool major = frame % 60 == 0;
        draw->AddLine(ImVec2(x, origin.y + (major ? 0 : ruler * .5f)), ImVec2(x, origin.y + laneHeight), IM_COL32(243, 235, 221, major ? 70 : 30));
        if (major) draw->AddText(ImVec2(x + 2 * unit, origin.y), IM_COL32(200, 190, 170, 200), std::to_string(frame).c_str());
    }
    static int dragging = -1; static float dragStartX = 0; static int dragStartFrame = 0;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = parsed[i];
        const float x0 = origin.x + (std::max)(0, step.at) * px, x1 = x0 + BlockFrames(step) * px;
        const ImVec2 top(x0, origin.y + ruler), bottom(x1, origin.y + ruler + blockHeight);
        const bool focused = i == focusedBlock && focus.compare(0, 4, "blk-") == 0;
        draw->AddRectFilled(top, bottom, focused ? IM_COL32(255, 135, 56, 200) : step.cancel ? IM_COL32(120, 90, 160, 170) : IM_COL32(70, 110, 150, 170), 3 * unit);
        if (focused) draw->AddRect(top, bottom, palette::Ivory, 3 * unit, 0, 2);
        // The press frame is the block's right edge, where the glyphs end.
        draw->AddLine(ImVec2(x1 - 3 * px, top.y), ImVec2(x1 - 3 * px, bottom.y), IM_COL32(255, 255, 255, 90));
        ImGui::PushClipRect(top, bottom, true);
        DrawComboStep(steps[i], ImVec2(x0 + 3 * unit, top.y + (blockHeight - line) / 2), IM_COL32(255, 255, 255, 240));
        ImGui::PopClipRect();
        // A press on a block takes it; a drag moves it by whole frames.
        const bool over = ImGui::IsWindowHovered() && mouse.x >= top.x && mouse.x <= bottom.x && mouse.y >= top.y && mouse.y <= bottom.y;
        if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { dragging = static_cast<int>(i); dragStartX = mouse.x; dragStartFrame = step.at; nav.Focus("blk-" + std::to_string(i), entries); }
    }
    if (dragging >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragging < static_cast<int>(steps.size())) {
        const int target = (std::max)(0, dragStartFrame + static_cast<int>((mouse.x - dragStartX) / px));
        if (target != parsed[dragging].at) changed |= NudgePattern(steps, static_cast<std::size_t>(dragging), target - parsed[dragging].at);
    } else dragging = -1;
    if (focusedBlock < steps.size() && focus.compare(0, 4, "blk-") == 0 && focus != "blk-add" && focus != "blk-delete" && focus != "blk-replay") {
        const float x0 = (std::max)(0, parsed[focusedBlock].at) * px, x1 = x0 + BlockFrames(parsed[focusedBlock]) * px;
        if (x0 < ImGui::GetScrollX()) ImGui::SetScrollX(x0);
        else if (x1 > ImGui::GetScrollX() + ImGui::GetWindowSize().x) ImGui::SetScrollX(x1 - ImGui::GetWindowSize().x);
        draw->AddText(ImVec2(origin.x + ImGui::GetScrollX() + 4 * unit, origin.y + ruler + blockHeight + 4 * unit), IM_COL32(220, 206, 166, 255),
            (std::to_string(focusedBlock + 1) + ". " + steps[focusedBlock]).c_str());
    }
    ImGui::Dummy(ImVec2(width, laneHeight));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (steps.empty()) ImGui::TextDisabled("%s", loc::T("training.combo.blocks.empty"));
    // The rows after the blocks, as the list would show them.
    for (const auto& entry : entries) {
        if (entry.id != "blk-add" && entry.id != "blk-delete" && entry.id != "blk-replay") continue;
        const bool focused = entry.id == focus;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        if (focused) ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + rowHeight), IM_COL32(255, 135, 56, 60), 2 * unit);
        if (entry.enabled) ImGui::TextUnformatted(entry.label.c_str()); else ImGui::TextDisabled("%s", entry.label.c_str());
        if (focused) { ImGui::SameLine(); ImGui::TextDisabled("%s", entry.detail.c_str()); }
    }
    (void)height;
    return changed;
}
} }
