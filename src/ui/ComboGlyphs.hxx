#pragma once
#include "../training/ComboBook.hxx"
#include "InputGlyphs.hxx"
#include <imgui.h>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

// Combo steps drawn the way the game's Trial screens show a command: an
// arrow per direction, a disc per button in its strength colour, words for
// what has no picture (xx, FADC, cl., mash). Drawn with the draw list, so no
// glyph has to exist in a font.
namespace sf4e { namespace ui {
namespace glyphs {
inline float Word(ImDrawList* draw, ImVec2 at, float h, const char* text, ImU32 tint) {
    const auto extent = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(at.x, at.y + (h - extent.y) / 2), tint, text);
    return extent.x;
}
}
// One canonical step at `at`, one text line high. Returns the width used.
// tint supplies the alpha and the colour of arrows and words; the discs keep
// their strength colours (light blue, medium yellow, heavy red, any white).
// mirror: the arrows as a fighter facing left presses them, 236 drawn as 214.
inline float DrawComboStep(const std::string& step, ImVec2 at, ImU32 tint, bool mirror = false) {
    using namespace glyphs;
    auto* draw = ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight(), gap = h * .12f;
    combo::Step parsed; std::string error;
    if (!combo::ParseStep(step, parsed, error)) return Word(draw, at, h, step.c_str(), tint);
    float x = at.x;
    const auto word = [&](const char* text) { x += Word(draw, ImVec2(x, at.y), h, text, tint) + gap; };
    if (parsed.follow) word("~"); else if (parsed.cancel) word("xx");
    if (step.compare(parsed.cancel ? 3 : 0, 4, "FADC") == 0) {
        word("FADC"); if (parsed.motion == "44") word("44");
        if (parsed.at >= 0) word(("#" + std::to_string(parsed.at)).c_str());
        if (parsed.offset) word(((parsed.offset > 0 ? "@+" : "@") + std::to_string(parsed.offset)).c_str());
        return x - at.x;
    }
    if (parsed.air) word("j.");
    if (parsed.range == combo::Range::Close) word("cl.");
    else if (parsed.range == combo::Range::Far) word("far.");
    else if (parsed.motion == "360" || parsed.motion == "720") word(parsed.motion.c_str());
    else for (std::size_t i = 0; i < parsed.motion.size(); ++i) {
        if (parsed.motion[i] == '5') continue;
        const char way = parsed.motion[i], column = static_cast<char>((way - '1') % 3);
        x += Arrow(draw, ImVec2(x, at.y), h, mirror && way >= '1' && way <= '9' ? static_cast<char>(way + 2 - 2 * column) : way, parsed.charge && i == 0, tint) + gap;
    }
    if (parsed.buttons) {
        if (parsed.edge == combo::Edge::Hold) word("[");
        if (parsed.edge == combo::Edge::Release) word("]");
        const bool any = parsed.buttons == combo::Punches || parsed.buttons == combo::Kicks;
        const char kind = parsed.buttons & combo::Punches ? 'P' : 'K';
        static const unsigned masks[] = {combo::LP, combo::MP, combo::HP, combo::LK, combo::MK, combo::HK};
        static const ImU32 colours[] = {IM_COL32(96, 176, 255, 255), IM_COL32(255, 214, 72, 255), IM_COL32(255, 96, 96, 255)};
        int drawn = 0;
        if (any) for (int i = 0; i < parsed.need; ++i) x += Disc(draw, ImVec2(x, at.y), h, kind, IM_COL32(228, 228, 228, 255), tint) + gap;
        else for (int i = 0; i < 6; ++i) if (parsed.buttons & masks[i]) {
            if (drawn++) word("+");
            x += Disc(draw, ImVec2(x, at.y), h, i < 3 ? 'P' : 'K', colours[i % 3], tint) + gap;
        }
        if (parsed.edge == combo::Edge::Hold) word("]");
        if (parsed.edge == combo::Edge::Release) word("[");
    }
    if (parsed.mash) word("(mash)");
    if (parsed.at >= 0) word(("#" + std::to_string(parsed.at)).c_str());
    if (parsed.offset) word(((parsed.offset > 0 ? "@+" : "@") + std::to_string(parsed.offset)).c_str());
    return x - at.x - gap;
}
// Steps in a row at the cursor, wrapping within `width`, as a layout item.
inline void ComboLine(const std::vector<std::string>& steps, float width, ImU32 tint) {
    const float h = ImGui::GetTextLineHeight(), gap = h * .5f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    float x = 0, y = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        // Measured by drawing off screen first; a step never splits across lines.
        const float w = DrawComboStep(steps[i], ImVec2(-10000, -10000), 0);
        if (x > 0 && x + w > width) { x = 0; y += h + h * .2f; }
        DrawComboStep(steps[i], ImVec2(origin.x + x, origin.y + y), tint);
        x += w + gap;
    }
    ImGui::Dummy(ImVec2(width, y + h));
}
} }
