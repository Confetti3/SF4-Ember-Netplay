#pragma once
#include "../training/ComboBook.hxx"
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
inline ImU32 Tinted(ImU32 colour, ImU32 tint) {
    return (colour & ~IM_COL32_A_MASK) | (tint & IM_COL32_A_MASK);
}
// Direction digits on a numpad, 6 pointing right; y grows downward on screen.
inline float Angle(char digit) {
    switch (digit) {
    case '6': return 0; case '9': return -.25f; case '8': return -.5f; case '7': return -.75f;
    case '4': return 1; case '1': return .75f; case '2': return .5f; default: return .25f;
    }
}
inline float Arrow(ImDrawList* draw, ImVec2 at, float h, char digit, bool held, ImU32 tint) {
    const float a = Angle(digit) * 3.14159265f, c = std::cos(a), s = std::sin(a);
    const ImVec2 centre(at.x + h / 2, at.y + h / 2);
    const float len = h * .38f, head = h * .22f, stem = h * .11f;
    const auto p = [&](float x, float y) { return ImVec2(centre.x + x * c - y * s, centre.y + x * s + y * c); };
    draw->AddLine(p(-len, 0), p(len - head, 0), tint, stem);
    draw->AddTriangleFilled(p(len, 0), p(len - head, -head), p(len - head, head), tint);
    // A held direction sits in a box, as the game's charge icons do.
    if (held) draw->AddRect(at, ImVec2(at.x + h, at.y + h), tint, 2, 0, 1);
    return h;
}
inline float Disc(ImDrawList* draw, ImVec2 at, float h, char kind, ImU32 colour, ImU32 tint) {
    const ImVec2 centre(at.x + h / 2, at.y + h / 2);
    draw->AddCircleFilled(centre, h * .46f, Tinted(colour, tint));
    draw->AddCircle(centre, h * .46f, Tinted(IM_COL32(20, 20, 20, 255), tint), 0, 1);
    const char text[2] = {kind, 0};
    const float size = h * .72f;
    const auto extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text);
    draw->AddText(ImGui::GetFont(), size, ImVec2(centre.x - extent.x / 2, centre.y - extent.y / 2), Tinted(IM_COL32(16, 16, 16, 255), tint), text);
    return h;
}
inline float Word(ImDrawList* draw, ImVec2 at, float h, const char* text, ImU32 tint) {
    const auto extent = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(at.x, at.y + (h - extent.y) / 2), tint, text);
    return extent.x;
}
}
// One canonical step at `at`, one text line high. Returns the width used.
// tint supplies the alpha and the colour of arrows and words; the discs keep
// their strength colours (light blue, medium yellow, heavy red, any white).
inline float DrawComboStep(const std::string& step, ImVec2 at, ImU32 tint) {
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
        x += Arrow(draw, ImVec2(x, at.y), h, parsed.motion[i], parsed.charge && i == 0, tint) + gap;
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
