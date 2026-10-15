#include "TrainingPanel.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuDialogs.hxx"
#include "MenuProbes.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <string>
#include <vector>

// The strip of keys over the frame meter: what the lab answers and the chip
// that opens its controls. One line, laid out from a measured model before
// anything is drawn.
namespace sf4e { namespace ui {
namespace {
// Never smaller than this on screen, whatever the HUD's size, the window's
// or the DPI: at the smallest HUD size a 720p window would otherwise draw
// it under 12 pixels.
constexpr float MinText = 15.f;
// Spacing in pixels at 15-pixel text, scaled with it: inside a cap and the
// chip, between a key and its label, and between one hint and the next.
constexpr float CapPadding = 4, PillPadding = 6, ChipPadding = 4, Gap = 4, Between = 12;
// One thing on the line, and the space in front of it. A key is named on an
// outlined cap and a pad's button on a pill: the prompt art's own lettering
// would be a few pixels tall at this size.
struct Piece {
    enum Kind { Key, Button, Plus, Chip, Label, Notice } kind;
    std::string text;
    float before;
};
// What goes on the line together: a key and what it does, or the controls'
// buttons and chip. A group shows whole, or, if it shortens, with its last
// piece cut to the room left.
struct Group { std::vector<Piece> pieces; bool shortens; };
struct Layout { float unit, least; };
float Width(const Piece& piece, const Layout& layout) {
    const float text = ImGui::CalcTextSize(piece.text.c_str()).x;
    switch (piece.kind) {
    case Piece::Key: return text + 2 * CapPadding * layout.unit;
    case Piece::Button: return text + 2 * PillPadding * layout.unit;
    case Piece::Chip: return text + 2 * ChipPadding * layout.unit;
    default: return text;
    }
}
// The groups in order of need, so what is left out for room is the least
// needed: the controls, then the pad's Back or the keys that record and play,
// then the position keys the player picked and F5. F7 and F8 read Stop while
// they record and play.
std::vector<Group> Groups(const training::View& view, bool shortcuts, const std::string& notice, int device, float unit) {
    std::vector<Group> groups;
    const bool pad = device == input::PadXInput || device == input::PadDirectInput;
    // A DirectInput pad's labels are unknown, so its Back is named Select.
    const std::string back = device == input::PadXInput ? "Back" : "Select";
    const auto hint = [&](Piece::Kind kind, std::string key, const char* label, bool shortens = false) {
        groups.push_back({{{kind, std::move(key), Between * unit}, {Piece::Label, label, Gap * unit}}, shortens});
    };
    if (shortcuts) {
        Group controls{{}, true};
        if (pad) controls.pieces = {{Piece::Button, back, 0}, {Piece::Plus, "+", 2 * unit}, {Piece::Button, "Start", 2 * unit}};
        else controls.pieces = {{Piece::Key, "F6", 0}};
        controls.pieces.push_back({Piece::Chip, loc::T("training.controls_chip"), Gap * unit});
        groups.push_back(controls);
    }
    // What a position key just did takes the place of the hints.
    if (!notice.empty()) { groups.push_back({{{Piece::Notice, notice, Between * unit}}, true}); return groups; }
    // The pad's Back is its one way to reset or save, so it is shortened to stay.
    if (pad) hint(Piece::Button, back, loc::T("training.pad_hint"), true);
    hint(Piece::Key, TrainingKeyName(TrainingRecordKey), loc::T(view.mode == training::Mode::Recording ? "training.stop" : "training.record"));
    hint(Piece::Key, TrainingKeyName(TrainingPlayKey), loc::T(view.mode == training::Mode::Playback ? "training.stop" : "training.play"));
    if (!pad) {
        for (const auto& key : TrainingPositionKeys()) hint(Piece::Key, key.key, loc::T(key.label));
        hint(Piece::Key, "F5", loc::T("training.hide_hint"));
    }
    return groups;
}
// The groups that fit in room, in order, the first on the line without
// space in front. Each goes whole; the first that does not end the line,
// shortened to the room left if it shortens and keeps at least `least` of
// its last piece's text.
std::vector<Piece> Fit(std::vector<Group> groups, float room, const Layout& layout, float& used) {
    std::vector<Piece> line;
    used = 0;
    for (auto& group : groups) {
        if (line.empty()) group.pieces.front().before = 0;
        float width = 0;
        for (const auto& piece : group.pieces) width += piece.before + Width(piece, layout);
        auto& last = group.pieces.back();
        const float text = ImGui::CalcTextSize(last.text.c_str()).x, spare = room - used - (width - text);
        const bool whole = text <= spare;
        if (!whole && !(group.shortens && spare >= layout.least)) break;
        if (!whole) last.text = FitLabel(last.text, spare);
        used += width - text + ImGui::CalcTextSize(last.text.c_str()).x;
        line.insert(line.end(), group.pieces.begin(), group.pieces.end());
        if (!whole) break;
    }
    return line;
}
void DrawCap(const Piece& piece, const Layout& layout) {
    const bool pill = piece.kind == Piece::Button;
    const float h = ImGui::GetTextLineHeight(), w = Width(piece, layout), unit = layout.unit;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    // The name a little brighter than the dim label beside it, so the key is what the eye finds.
    draw->AddRect(ImVec2(at.x + .5f, at.y + .5f), ImVec2(at.x + w - .5f, at.y + h - .5f), IM_COL32(243, 235, 221, 130),
        pill ? h / 2 : 3 * unit, 0, (std::max)(1.f, unit));
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + (pill ? PillPadding : CapPadding) * unit, at.y), IM_COL32(243, 235, 221, 215), piece.text.c_str());
    ImGui::Dummy(ImVec2(w, h));
}
TrainingStripShown shown;
}
const TrainingStripShown& LastTrainingStrip() { return shown; }
TrainingHudInput DrawTrainingShortcuts(const training::View& view, bool called, float left, float meterTop, float width, float hudScale) {
    shown = {};
    TrainingHudInput input;
    bool failed = false;
    const auto notice = TrainingNotice(failed);
    const bool shortcuts = TrainingMeterOptions().shortcuts;
    // Under the call back from a room the controls cannot open, so the chip
    // and its pad hints would be wrong; the call's banner says what can be
    // pressed then. With the shortcuts off only a notice is told.
    if (called || (!shortcuts && notice.empty())) return input;
    // At least MinText pixels, otherwise as the meter's own text.
    const float body = ImGui::GetFont()->FontSize, text = (std::max)(MinText, .8f * body * hudScale);
    const float unit = text / MinText;
    const ImVec2 padding(4 * unit, 3 * unit);
    ImGui::SetNextWindowPos(ImVec2(left, meterTop - 4 * hudScale), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(TrainingHudBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("Training shortcuts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::SetWindowFontScale(text / (body * Scale()));
        // The least a shortened piece keeps: a letter or two and "...".
        const Layout layout{unit, ImGui::CalcTextSize("WW...").x + 1};
        const float room = width - 2 * padding.x;
        float used = 0;
        const auto line = Fit(Groups(view, shortcuts, notice, MenuPromptDevice(), unit), room, layout, used);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ChipPadding * unit, 0));
        for (std::size_t i = 0; i < line.size(); ++i) {
            const auto& piece = line[i];
            if (i) ImGui::SameLine(0, piece.before);
            switch (piece.kind) {
            case Piece::Key: case Piece::Button: DrawCap(piece, layout); break;
            case Piece::Chip:
                input.open = ImGui::SmallButton(piece.text.c_str());
                ReportMenuCard("training-open", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                break;
            case Piece::Notice:
                ImGui::TextColored(failed ? ToneColor(Tone::Error) : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s", piece.text.c_str());
                break;
            default: ImGui::TextDisabled("%s", piece.text.c_str()); break;
            }
            shown.pieces.push_back(piece.text);
        }
        ImGui::PopStyleVar();
        shown.textPixels = ImGui::GetFontSize(); shown.width = used; shown.room = room;
        ReportMenuText("training-shortcuts", text, ImGui::GetTextLineHeight(), used, room);
        ImGui::SetWindowFontScale(1.f);
        input.pointer = ImGui::IsWindowHovered();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return input;
}
} }
