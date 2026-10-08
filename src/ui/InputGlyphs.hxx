#pragma once
#include <imgui.h>
#include <cfloat>
#include <cmath>

// Pad inputs drawn the way the game shows a command: an arrow per direction,
// a disc per button in its strength colour. Drawn with the draw list, so no
// glyph has to exist in a font.
namespace sf4e { namespace ui { namespace glyphs {
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
} } }
