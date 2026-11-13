#include "V2.hxx"
#include "LineIcons.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuGlyphs.hxx"
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace sf4e { namespace ui { namespace v2 {
namespace {
float TextWidth(const std::string& text, float size, ImFont* font = ImGui::GetFont()) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; }
// A tag's link mark, square, a little smaller than the ping chip beside it.
float LinkGlyph() { return ChipFont() + 4 * Scale(); }
}
float ChipFont() { return ImGui::GetFontSize() * .78f; }
std::string Fit(const std::string& text, float width, float size, ImFont* font, bool probe) {
    if (!font) font = ImGui::GetFont();
    const float measured = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x;
    // The text probe sees every line drawn here at its full width, so the lab's check finds what this cuts.
    if (probe) ReportMenuText(("v2/" + text).c_str(), size, size, measured, width);
    if (measured <= width) return text;
    const char* end = nullptr;
    font->CalcTextSizeA(size, (std::max)(0.f, width - font->CalcTextSizeA(size, FLT_MAX, 0, "...").x), 0, text.c_str(), nullptr, &end);
    return std::string(text.c_str(), end) + "...";
}

Share ShareLine(float label, float value, float least, float room, float gap, bool grows) {
    if (value <= 0) return {};
    if (label + gap + value <= room) return {value, false};
    if (label + gap + least <= room) return {room - label - gap, false};
    if (!grows) return {(std::max)(0.f, room - label - gap), false};
    return {(std::min)(value, room), true};
}

namespace {
constexpr Palette EmberPalette{IM_COL32(20, 19, 18, 255), IM_COL32(25, 23, 21, 255), IM_COL32(33, 30, 27, 255), IM_COL32(56, 45, 36, 255),
    IM_COL32(58, 42, 31, 255), IM_COL32(89, 64, 43, 255), IM_COL32(68, 58, 49, 255), IM_COL32(255, 135, 56, 255), IM_COL32(255, 177, 111, 255),
    IM_COL32(243, 235, 221, 255), IM_COL32(181, 169, 155, 255), IM_COL32(255, 92, 80, 255), IM_COL32(255, 170, 160, 255), IM_COL32(164, 206, 160, 255)};
}
const Palette& Colors() { return EmberPalette; }

void Focus(ImDrawList* draw, ImVec2 min, ImVec2 max, bool bar) {
    const float s = Scale(), r = 4 * s;
    draw->AddRect(ImVec2(min.x + s, min.y + s), ImVec2(max.x - s, max.y - s), Colors().accent, r, 0, 2 * s);
    if (bar) draw->AddRectFilled(min, ImVec2(min.x + 4 * s, max.y), Colors().accent, r, ImDrawFlags_RoundCornersLeft);
}

void CardGround(ImDrawList* draw, Box card, bool focused, bool hovered, bool act) {
    const float s = Scale();
    draw->AddRectFilled(card.min, card.max, focused ? Colors().header : hovered ? Colors().hover : color::Alpha(Colors().frame, .92f), 4 * s);
    draw->AddRect(card.min, card.max, color::Alpha(Colors().border, .6f), 4 * s, 0, Hairline());
    if (act) draw->AddRectFilled(card.min, ImVec2(card.min.x + 3 * s, card.max.y), Colors().accent, 4 * s, ImDrawFlags_RoundCornersLeft);
}

void RowGround(ImDrawList* draw, Box row, bool focused, bool hovered) {
    if (focused || hovered) draw->AddRectFilled(row.min, row.max, focused ? Colors().header : Colors().hover, 4 * Scale());
}

namespace {
// A press on `b` without an item: the left button let go over it, where it
// went down, in this window with nothing over it. The hint bar sits on the
// window's bottom edge, past its padding, where an item would be content the
// window scrolls to; it is the board's own edge, not content.
struct EdgePress { bool hovered, pressed; };
EdgePress PressedOn(Box b) {
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(b.min, b.max);
    const ImVec2 down = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
    const bool began = down.x >= b.min.x && down.x < b.max.x && down.y >= b.min.y && down.y < b.max.y;
    return {hovered, hovered && began && ImGui::IsMouseReleased(ImGuiMouseButton_Left)};
}
}
int HintBar(Box bar, const std::vector<Hint>& hints, const std::string& status, ImU32 tone, bool bell) {
    const float s = Scale(), glyph = (bar.H() < 52 * s ? 26 : 30) * s, font = 16 * s;
    auto* d = ImGui::GetWindowDrawList();
    const float side = SideMargin(ImGui::GetWindowWidth(), s);
    d->AddRectFilled(bar.min, bar.max, color::Alpha(Colors().ink, .92f));
    d->AddLine(bar.min, ImVec2(bar.max.x, bar.min.y), Colors().border, Hairline());
    int pressed = -1;
    float x = bar.min.x + side;
    const float right = bar.max.x - side, size = 15 * s, icon = bell ? 30 * s : 0;
    // When the hints and the status do not both fit (a narrow screen), the hints
    // keep their glyphs only and the status keeps its words.
    float wanted = TextWidth(status, size) + icon;
    for (const auto& h : hints) wanted += glyph + 32 * s + ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0, PromptLabel(h.glyph, h.label, MenuPromptBrand()).c_str()).x;
    const bool compact = x + wanted > right;
    for (int i = 0; i < static_cast<int>(hints.size()); ++i) {
        const auto& h = hints[i];
        const std::string label = compact ? std::string() : PromptLabel(h.glyph, h.label, MenuPromptBrand());
        const float w = glyph + (label.empty() ? 0 : 8 * s + ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0, label.c_str()).x);
        if (!h.labelOnly && PressedOn({ImVec2(x, bar.min.y), ImVec2(x + w, bar.max.y)}).pressed) pressed = i;
        DrawPromptGlyph(d, h.glyph, ImVec2(x, bar.min.y + (bar.H() - glyph) * .5f), glyph, MenuArt(), s);
        d->AddText(ImGui::GetFont(), font, ImVec2(x + glyph + 8 * s, bar.min.y + (bar.H() - font) * .5f), h.labelOnly ? Colors().muted : Colors().text, label.c_str());
        x += w + (compact ? 12 : 24) * s;
    }
    // The status takes a second line where one does not hold it (Home's
    // version on a narrow screen), broken at the last space that lets the
    // first fit, and is cut to the bar's room only beyond that. The probe
    // measures labels, as a status was never measured.
    const float room = (std::max)(0.f, right - x - icon), mid = bar.min.y + bar.H() * .5f;
    std::string first = status, second;
    for (auto space = status.rfind(' '); TextWidth(status, size) > room && space != std::string::npos && space > 0; space = status.rfind(' ', space - 1))
        if (TextWidth(status.substr(0, space), size) <= room) { first = status.substr(0, space); second = status.substr(space + 1); break; }
    const std::string top = Fit(first, room, size, nullptr, false), bottom = Fit(second, room, size, nullptr, false);
    const float shown = (std::max)(TextWidth(top, size), TextWidth(bottom, size)), left = right - shown - icon;
    const float line = bottom.empty() ? 0 : size * .6f;
    d->AddText(ImGui::GetFont(), size, ImVec2(right - TextWidth(top, size), mid - size * .5f - line), tone, top.c_str());
    if (!bottom.empty()) d->AddText(ImGui::GetFont(), size, ImVec2(right - TextWidth(bottom, size), mid - size * .5f + line), tone, bottom.c_str());
    if (bell) {
        // The bell of the Toasts board's touch control.
        const EdgePress bellPress = PressedOn({ImVec2(left, bar.min.y), ImVec2(right, bar.max.y)});
        if (bellPress.pressed) pressed = static_cast<int>(hints.size());
        const float k = s * .8f;
        DrawLineIcon(d, LineIcon::Bell, ImVec2(left, mid - 11 * k), 22 * k, bellPress.hovered ? Colors().accent : Colors().text);
    }
    return pressed;
}

void Glow(ImDrawList* d, ImVec2 min, ImVec2 max, ImU32 color, float strength, float reach, float rounding) {
    const int rings = 6;
    for (int i = 1; i <= rings; ++i) {
        const float out = reach * i / rings;
        d->AddRect(ImVec2(min.x - out, min.y - out), ImVec2(max.x + out, max.y + out),
            color::Alpha(color, strength * (1 - (i - 1) / static_cast<float>(rings))), rounding + out, 0, reach / rings + 1);
    }
}

int SpotMap(const char* id, const std::vector<ImVec2>& spots, int current, ImVec2 block) {
    const float s = Scale(), w = (std::min)(300 * s, ImGui::GetContentRegionAvail().x), h = w * 9 / 16;
    const auto p = ImGui::GetCursorScreenPos();
    auto* d = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool held = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Colors().frame, 4 * s);
    d->AddRect(p, ImVec2(p.x + w, p.y + h), hovered || held ? Colors().accent : Colors().border, 4 * s, 0, Hairline());
    const ImVec2 size(block.x * w, block.y * h);
    const auto at = [&](ImVec2 centre) { return ImVec2(p.x + centre.x * w - size.x * .5f, p.y + centre.y * h - size.y * .5f); };
    const auto mouse = ImGui::GetMousePos();
    // The spot nearest the pointer, where a drop lands.
    int nearest = 0;
    float best = FLT_MAX;
    for (int i = 0; i < static_cast<int>(spots.size()); ++i) {
        const float dx = p.x + spots[i].x * w - mouse.x, dy = p.y + spots[i].y * h - mouse.y;
        if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; nearest = i; }
    }
    for (int i = 0; i < static_cast<int>(spots.size()); ++i) {
        const auto q = at(spots[i]);
        d->AddRect(q, ImVec2(q.x + size.x, q.y + size.y), held && i == nearest ? Colors().accent : color::Alpha(Colors().muted, .5f), 2 * s, 0, Hairline());
    }
    const ImVec2 q = held ? ImVec2(mouse.x - size.x * .5f, mouse.y - size.y * .5f) : at(spots[(std::max)(0, (std::min)(static_cast<int>(spots.size()) - 1, current))]);
    d->AddRectFilled(q, ImVec2(q.x + size.x, q.y + size.y), Colors().accent, 2 * s);
    return ImGui::IsItemDeactivated() && hovered ? nearest : -1;
}

float Grip(const char* id, ImVec2 from, ImVec2 to, bool live) {
    const float s = Scale(), reach = 22 * s;
    const bool down = from.x == to.x;
    const ImVec2 mid((from.x + to.x) * .5f, (from.y + to.y) * .5f);
    const ImVec2 at = down ? ImVec2(from.x - reach, from.y) : ImVec2(from.x, from.y - reach);
    const ImVec2 size = down ? ImVec2(2 * reach, (std::max)(1.f, to.y - from.y)) : ImVec2((std::max)(1.f, to.x - from.x), 2 * reach);
    ReportMenuCard(id, at, ImVec2(at.x + size.x, at.y + size.y));
    // Its own child window: the panels beside it are child windows too, which a
    // draw list of the board's would be drawn under, covering the dot's halves.
    ImGui::SetCursorScreenPos(at);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.f);
    ImGui::BeginChild(id, size, 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoNavInputs | (live ? 0 : ImGuiWindowFlags_NoMouseInputs));
    bool held = false, hovered = false;
    if (live) {
        ImGui::InvisibleButton(id, size);
        held = ImGui::IsItemActive(); hovered = ImGui::IsItemHovered();
    }
    if (held || hovered) ImGui::SetMouseCursor(down ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = Colors();
    const ImU32 line = !live ? color::Alpha(c.muted, .35f) : held ? c.accent : hovered ? color::Alpha(c.accent, .7f) : color::Alpha(c.accent, .35f);
    d->AddLine(from, to, line, 4 * s);
    d->AddCircleFilled(mid, 18 * s, held ? c.accent : c.button);
    d->AddCircle(mid, 18 * s, line, 0, s);
    // Three dots at rest; held, the two ways it moves.
    const ImU32 ink = held ? c.ink : live ? c.text : c.muted;
    if (held) for (float side : {-1.f, 1.f}) {
        const ImVec2 tip = down ? ImVec2(mid.x + side * 10 * s, mid.y) : ImVec2(mid.x, mid.y + side * 10 * s);
        const ImVec2 a = down ? ImVec2(tip.x - side * 6 * s, mid.y - 6 * s) : ImVec2(mid.x - 6 * s, tip.y - side * 6 * s);
        const ImVec2 b = down ? ImVec2(tip.x - side * 6 * s, mid.y + 6 * s) : ImVec2(mid.x + 6 * s, tip.y - side * 6 * s);
        d->AddTriangleFilled(tip, a, b, ink);
    }
    else for (int i = -1; i <= 1; ++i) d->AddCircleFilled(down ? ImVec2(mid.x, mid.y + i * 6 * s) : ImVec2(mid.x + i * 6 * s, mid.y), 2 * s, ink);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    return held ? (down ? ImGui::GetMousePos().x : ImGui::GetMousePos().y) : NAN;
}

ImVec4 BoardGround(bool seeThrough) {
    return ImGui::ColorConvertU32ToFloat4(color::Alpha(Colors().panel, seeThrough ? .6f : .84f));
}

float ChipWidth(const std::string& text) { return TextWidth(text, ChipFont()) + 16 * Scale(); }
float Chip(ImDrawList* draw, ImVec2 at, const std::string& text, ImU32 tone) {
    const float s = Scale(), size = ChipFont(), width = ChipWidth(text);
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + size + 8 * s), color::Alpha(tone, .18f), 4 * s);
    draw->AddText(ImGui::GetFont(), size, ImVec2(at.x + 8 * s, at.y + 4 * s), tone, text.c_str());
    return width;
}

float Paragraph(ImDrawList* draw, ImVec2 at, float width, const std::string& text, float size, ImU32 color, ImFont* font) {
    const float line = size * 1.45f;
    float y = at.y;
    // ImGui's wrap runs on through a newline, so each line is wrapped up to its own.
    for (const char *p = text.c_str(), *end = p + text.size(); p < end; y += line) {
        const char* stop = std::find(p, end, '\n');
        const char* cut = p == stop ? stop : font->CalcWordWrapPositionA(size / font->FontSize, p, stop, width);
        if (cut == p && p != stop) ++cut;  // ponytail: a pane narrower than one glyph splits it
        if (draw) draw->AddText(font, size, ImVec2(at.x, y + (line - size) * .5f), color, p, cut);
        for (p = cut; p < stop && *p == ' '; ++p) {}
        if (p == stop && p < end) ++p;
    }
    return y - at.y;
}
void Paragraph(const std::string& text, float size, ImU32 color, ImFont* font) {
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(width, Paragraph(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), width, text, size, color, font)));
}

ImU32 PanelGround(bool seeThrough) { return color::Alpha(Colors().panel, seeThrough ? .78f : .94f); }
Box Panel(ImDrawList* draw, Box b, const char* title, const char* trailing, bool open) {
    const float s = Scale(), size = 20 * s, room = b.W() - 32 * s;
    const auto& c = Colors();
    const float tw = trailing ? HeadWord(title, size, trailing, room, 16 * s) : 0, titleRoom = tw > 0 ? room - 16 * s - tw : room;
    draw->AddRectFilled(b.min, b.max, PanelGround(), 4 * s);
    const Box head = CutTop(b, 20 * s + HeadingHeight(title, titleRoom, size));
    if (open) draw->AddRectFilled(head.min, head.max, c.header, 4 * s, ImDrawFlags_RoundCornersTop);
    draw->AddLine(ImVec2(head.min.x, head.max.y), head.max, c.border, Hairline());
    draw->AddRect(head.min, b.max, open ? c.accentLight : c.border, 4 * s, 0, Hairline());
    if (tw > 0) draw->AddText(ImVec2(head.max.x - 16 * s - tw, head.min.y + (head.H() - ImGui::GetFontSize()) * .5f), c.muted, trailing);
    Heading(draw, ImVec2(head.min.x + 16 * s, head.min.y + 10 * s), titleRoom, title, size, c.text);
    return b;
}
float SectionHeading(ImDrawList* draw, ImVec2 at, const std::string& text, ImU32 tone) {
    draw->AddText(HeadingFont(), 14 * Scale(), at, tone, text.c_str());
    return TextWidth(text, 14 * Scale(), HeadingFont());
}
float Text(ImDrawList* draw, ImVec2 at, float width, const std::string& text, float size, ImU32 color, ImFont* font, bool centred, bool probe) {
    if (!font) font = ImGui::GetFont();
    const std::string shown = Fit(text, width, size, font, probe);
    const float w = TextWidth(shown, size, font);
    draw->AddText(font, size, ImVec2(centred ? at.x + (width - w) * .5f : at.x, at.y), color, shown.c_str());
    return w;
}
float TextShrunk(ImDrawList* draw, ImVec2 at, float width, const std::string& text, float size, float least, ImU32 color, ImFont* font) {
    if (!font) font = ImGui::GetFont();
    const float measured = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x;
    const float used = measured > width ? (std::max)(least, size * width / measured) : size;
    return Text(draw, ImVec2(at.x, at.y + (size - used) * .5f), width, text, used, color, font, false, false);
}
namespace {
// A heading's two lines in `width`: all of it on the first when it fits, else
// the most whole words that fit (the first word at least) and the rest.
std::pair<std::string, std::string> HeadingLines(const std::string& text, float width, float size, ImFont* font) {
    std::size_t cut = text.find(' ');
    if (TextWidth(text, size, font) <= width || cut == std::string::npos) return {text, {}};
    for (std::size_t next = text.find(' ', cut + 1); next != std::string::npos && TextWidth(text.substr(0, next), size, font) <= width; next = text.find(' ', next + 1)) cut = next;
    const std::size_t rest = text.find_first_not_of(' ', cut);
    if (rest == std::string::npos) return {text, {}};
    return {text.substr(0, cut), text.substr(rest)};
}
}
float HeadWord(const std::string& heading, float size, const std::string& word, float room, float gap) {
    const float w = TextWidth(word, ImGui::GetFontSize());
    return TextWidth(heading, size, HeadingFont()) + gap + w <= room ? w : 0;
}
float HeadingHeight(const std::string& text, float width, float size, ImFont* font) {
    return HeadingLines(text, width, size, font ? font : HeadingFont()).second.empty() ? size : size * (1 + HeadingLine);
}
float Heading(ImDrawList* draw, ImVec2 at, float width, const std::string& text, float size, ImU32 color, ImFont* font) {
    if (!font) font = HeadingFont();
    const auto [first, second] = HeadingLines(text, width, size, font);
    // A heading with no space to break at (Japanese, or one long word) shrinks to its room before it is cut.
    if (second.empty() && TextWidth(first, size, font) > width) TextShrunk(draw, at, width, first, size, size * .75f, color, font);
    else Text(draw, at, width, first, size, color, font);
    if (second.empty()) return size;
    Text(draw, ImVec2(at.x, at.y + size * HeadingLine), width, second, size, color, font);
    return size * (1 + HeadingLine);
}
void Chevron(ImDrawList* draw, ImVec2 at, float size, ImU32 color) {
    const ImVec2 points[] = {{at.x, at.y - size}, {at.x + size * .6f, at.y}, {at.x, at.y + size}};
    draw->AddPolyline(points, 3, color, 0, 2 * Scale());
}
void Button(ImDrawList* draw, Box b, const std::string& label, ButtonKind kind, bool focused, bool hovered, bool enabled) {
    const float s = Scale(), size = 17 * s;
    const auto& c = Colors();
    const bool primary = kind == ButtonKind::Primary, danger = kind == ButtonKind::Danger;
    ImU32 fill = focused ? c.header : hovered ? c.hover : c.button, edge = color::Alpha(danger ? c.danger : c.accent, danger ? .5f : .35f);
    ImU32 ink = danger ? c.danger : c.text;
    if (!enabled) { fill = c.frame; edge = c.border; ink = c.muted; }
    else if (primary) { fill = focused || hovered ? c.accentLight : c.accent; edge = c.accent; ink = c.ink; }
    draw->AddRectFilled(b.min, b.max, fill, 4 * s);
    draw->AddRect(b.min, b.max, edge, 4 * s, 0, Hairline());
    if (!label.empty()) Text(draw, ImVec2(b.min.x + 12 * s, b.min.y + (b.H() - size) * .5f), b.W() - 24 * s, label, size, ink, HeadingFont(), true);
    if (focused && primary && enabled) draw->AddRect(b.min, b.max, c.text, 4 * s, 0, 3 * s);
    else if (focused) Focus(draw, b.min, b.max);
}
namespace {
// The places on line `line` of `lines`: the first and how many, shared out evenly in order.
std::pair<std::size_t, std::size_t> BarLine(std::size_t places, int lines, int line) {
    const std::size_t per = places / lines, extra = places % lines, at = static_cast<std::size_t>(line);
    return {at * per + (std::min)(at, extra), per + (at < extra)};
}
}
int BarLines(float width, const std::vector<float>& need, float gap) {
    const int places = static_cast<int>(need.size());
    for (int lines = 1; lines < places; ++lines) {
        bool fits = true;
        for (int line = 0; line < lines && fits; ++line) {
            const auto [first, count] = BarLine(need.size(), lines, line);
            float used = (count - 1) * gap;
            for (std::size_t i = first; i < first + count; ++i) used += need[i];
            fits = used <= width;
        }
        if (fits) return lines;
    }
    return (std::max)(1, places);
}
std::vector<Box> BarPlaces(Box bar, const std::vector<float>& need, float gap, int lines, float height) {
    std::vector<Box> places(need.size());
    for (int line = 0; line < lines; ++line) {
        const auto [first, count] = BarLine(need.size(), lines, line);
        if (!count) continue;
        // The even share, raised past what the places that need more take.
        std::vector<float> wants(need.begin() + first, need.begin() + first + count);
        std::sort(wants.begin(), wants.end(), std::greater<float>());
        float rest = bar.W() - (count - 1) * gap, share = rest / count;
        for (std::size_t k = 0; k < count && wants[k] > share; ++k) { rest -= wants[k]; share = rest / (count - k - 1 ? count - k - 1 : 1); }
        float x = bar.min.x;
        const float y = bar.min.y + line * (height + gap);
        for (std::size_t i = first; i < first + count; ++i) {
            const float w = (std::max)(need[i], share);
            places[i] = {ImVec2(x, y), ImVec2(x + w, y + height)};
            x += w + gap;
        }
    }
    return places;
}
void Words(ImDrawList* draw, Box b, const std::string& label, const std::string& detail, bool wrap) {
    const float s = Scale(), x = b.min.x + 16 * s, w = b.W() - 32 * s;
    const auto& c = Colors();
    draw->AddRectFilled(b.min, b.max, c.frame, 4 * s);
    draw->AddRect(b.min, b.max, c.border, 4 * s, 0, Hairline());
    Text(draw, ImVec2(x, b.min.y + 6 * s), w, label, 16 * s, c.text, HeadingFont());
    if (wrap) Paragraph(draw, ImVec2(x, b.min.y + 28 * s), w, detail, 15 * s, c.muted);
    else Text(draw, ImVec2(x, b.min.y + 28 * s), w, detail.substr(0, detail.find('\n')), 15 * s, c.muted);
}
float WordsHeight(float width, const std::string& detail) {
    const float s = Scale();
    return 36 * s + Paragraph(nullptr, ImVec2(), width - 32 * s, detail, 15 * s, 0);
}
void Wrapped(const std::string& text) {
    Paragraph(text, ImGui::GetFontSize(), ImGui::GetColorU32(ImGuiCol_Text));
}
void Fact(const std::string& key, const std::string& value) {
    const float s = Scale(), w = ImGui::GetContentRegionAvail().x, font = ImGui::GetFontSize(), room = w - 24 * s;
    const float wanted = TextWidth(value, font);
    const Share share = ShareLine(TextWidth(key, font), wanted, wanted, room, 16 * s);
    const float h = font + 20 * s + (share.under ? UnderLine * s : 0);
    const auto p = ImGui::GetCursorScreenPos();
    auto* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Colors().frame, 4 * s);
    d->AddRect(p, ImVec2(p.x + w, p.y + h), Colors().border, 4 * s, 0, Hairline());
    const std::string shown = Fit(value, share.value, font);
    const float vw = TextWidth(shown, font);
    Text(d, ImVec2(p.x + 12 * s, p.y + 10 * s), share.under ? room : room - vw - 16 * s, key, font, Colors().muted);
    d->AddText(ImVec2(p.x + w - 12 * s - vw, p.y + 10 * s + (share.under ? UnderLine * s : 0)), Colors().text, shown.c_str());
    ImGui::Dummy(ImVec2(w, h));
}

float BrandMark(ImDrawList* draw, ImVec2 at, float height) {
    auto* atlas = ImGui::GetIO().Fonts;
    ImVec2 uv0, uv1;
    atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(0), &uv0, &uv1);
    draw->AddImage(atlas->TexID, at, ImVec2(at.x + height, at.y + height), uv0, uv1);
    return height;
}

HeaderMeasures MeasureHeader(bool eyebrow, float windowWidth) {
    const float s = Scale();
    const bool narrow = Narrow(windowWidth, s);
    HeaderMeasures m;
    m.mark = (narrow ? 32 : 40) * s;
    m.title = (narrow ? 22 : 26) * s;
    m.eyebrow = 13 * s;
    m.eyebrowLine = eyebrow ? m.eyebrow + 4 * s : 0;
    m.band = (std::max)(m.mark, m.eyebrowLine + m.title) + 10 * s;
    m.height = m.band + 8 * s;
    return m;
}
bool Header(const char* title, const char* eyebrow, const std::vector<HeaderChip>& chips, const char* back) {
    const float s = Scale();
    const auto p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    auto* draw = ImGui::GetWindowDrawList();
    const HeaderMeasures measures = MeasureHeader(eyebrow != nullptr, ImGui::GetWindowWidth());
    const float markHeight = measures.mark, gap = 16 * s;
    const float eyebrowSize = measures.eyebrow, titleSize = measures.title;
    const float eyebrowHeight = measures.eyebrowLine;
    const float height = measures.band;
    // Back: a chevron and its word on a button, as the boards draw it.
    float left = p.x;
    bool pressed = false;
    if (back) {
        const float bh = 44 * s, bw = 34 * s + TextWidth(back, 16 * s) + 12 * s;
        const ImVec2 at(p.x, p.y + (height - 10 * s - bh) * .5f), to(at.x + bw, at.y + bh);
        ImGui::SetCursorScreenPos(at);
        pressed = ImGui::InvisibleButton("##header-back", ImVec2(bw, bh));
        Button(draw, {at, to}, {}, ButtonKind::Secondary, false, ImGui::IsItemHovered());
        const float mid = at.y + bh * .5f;
        const ImVec2 chevron[] = {{at.x + 21 * s, mid - 5 * s}, {at.x + 16 * s, mid}, {at.x + 21 * s, mid + 5 * s}};
        draw->AddPolyline(chevron, 3, Colors().text, 0, 2 * s);
        draw->AddText(ImGui::GetFont(), 16 * s, ImVec2(at.x + 34 * s, mid - 8 * s), Colors().text, back);
        ImGui::SetCursorScreenPos(p);
        left += bw + gap;
    }
    const float mark = BrandMark(draw, ImVec2(left, p.y + (height - 10 * s - markHeight) * .5f), markHeight), x = left + mark + gap;
    // The chips first, from the right end, so the title knows its room.
    float right = p.x + width;
    for (auto chip = chips.rbegin(); chip != chips.rend(); ++chip) {
        right -= ChipWidth(chip->text);
        Chip(draw, ImVec2(right, p.y + (height - 10 * s - ChipFont() - 8 * s) * .5f), chip->text, chip->tone);
        right -= 8 * s;
    }
    const float room = right - x;
    float y = p.y + (height - 10 * s - eyebrowHeight - titleSize) * .5f;
    if (eyebrow) {
        // The path gives way from the left a whole part at a time (EMBER ·
        // ONLINE · PRIVATE ROOM keeps PRIVATE ROOM), and is left out when not
        // even its last part fits; it is never cut.
        std::string path = eyebrow;
        while (!path.empty() && TextWidth(path, eyebrowSize) > room) {
            const auto part = path.find(" \xC2\xB7 ");
            path = part == std::string::npos ? std::string() : path.substr(part + 4);
        }
        if (!path.empty()) draw->AddText(ImGui::GetFont(), eyebrowSize, ImVec2(x, y), Colors().muted, Fit(path, room, eyebrowSize).c_str());
        y += eyebrowHeight;
    }
    // A title may be a player's own words (a room's name), so the probe leaves it out.
    draw->AddText(HeadingFont(), titleSize, ImVec2(x, y), Colors().text, Fit(title, room, titleSize, HeadingFont(), false).c_str());
    draw->AddLine(ImVec2(p.x, p.y + height), ImVec2(p.x + width, p.y + height), Colors().border, Hairline());
    draw->AddRectFilled(ImVec2(x, p.y + height - s), ImVec2(x + 72 * s, p.y + height + 2 * s), Colors().accent);
    ImGui::Dummy(ImVec2(width, measures.height));
    return pressed;
}

int Tabs(const std::vector<std::string>& labels, int current) {
    const float s = Scale(), height = 44 * s, pad = 16 * s;
    const auto origin = ImGui::GetCursorScreenPos();
    const float full = ImGui::GetContentRegionAvail().x;
    auto* draw = ImGui::GetWindowDrawList();
    int pressed = -1;
    // The device's tab buttons sit at the strip's ends (LB and RB, Page Up and
    // Down, a stick's LP and HP: MenuShortcutGlyph), each also a button to the
    // previous or next tab, wrapping at the ends (StepTab). A device without
    // one shows the keyboard's key, which steps the tabs too. Keyboard keys
    // are wider caps, so "PgUp" and "PgDn" read at full size.
    const char* glyphs[] = {MenuShortcutGlyph(MenuInput::PrevTab), MenuShortcutGlyph(MenuInput::NextTab)};
    if (!glyphs[0]) glyphs[0] = "PgUp";
    if (!glyphs[1]) glyphs[1] = "PgDn";
    const float cap = (KeyCapGlyph(glyphs[0]) ? 40 : 30) * s, capRoom = labels.size() > 1 ? cap + 12 * s : 0;
    if (capRoom > 0) {
        for (int side = 0; side < 2; ++side) {
            const ImVec2 at(side ? origin.x + full - cap : origin.x, origin.y + (height - cap) * .5f);
            ImGui::SetCursorScreenPos(at);
            ImGui::PushID(side ? "##next-tab" : "##previous-tab");
            if (ImGui::InvisibleButton("##cap", ImVec2(cap, cap))) pressed = StepTab(current, side ? 1 : -1, static_cast<int>(labels.size()));
            ImGui::PopID();
            DrawPromptGlyph(draw, glyphs[side], at, cap, MenuArt(), s, 0, MenuPromptBrand());
        }
    }
    const ImVec2 start(origin.x + capRoom, origin.y);
    const float width = full - 2 * capRoom;
    // A strip wider than the screen slides so the current tab stays in view.
    float end = 0, total = 0;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        total += ImGui::CalcTextSize(labels[i].c_str()).x + 2 * pad;
        if (i == current) end = total;
    }
    float x = start.x - (std::max)(0.f, (std::min)(end - width + pad, total - width));
    draw->PushClipRect(start, ImVec2(start.x + width, start.y + height + s), true);
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const float w = ImGui::CalcTextSize(labels[i].c_str()).x + 2 * pad;
        // A tab that does not fit whole stays out instead of showing a cut word; the cap steps to it.
        if (x < start.x - s || x + w > start.x + width + s) { x += w; continue; }
        ImGui::SetCursorScreenPos(ImVec2(x, start.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(w, height))) pressed = i;
        ImGui::PopID();
        const bool selected = i == current;
        if (selected) {
            draw->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + height), Colors().header);
            draw->AddRectFilled(ImVec2(x, start.y + height - 3 * s), ImVec2(x + w, start.y + height), Colors().accent);
        } else if (ImGui::IsItemHovered()) draw->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + height), Colors().hover);
        draw->AddText(ImVec2(x + pad, start.y + (height - ImGui::GetFontSize()) * .5f), selected ? Colors().text : Colors().muted, labels[i].c_str());
        x += w;
    }
    draw->PopClipRect();
    draw->AddLine(ImVec2(origin.x, start.y + height), ImVec2(origin.x + full, start.y + height), Colors().border, Hairline());
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(full, height + 8 * s));
    return pressed;
}

TierColors TierColor(Tier tier) {
    switch (tier) {
    case Tier::Ash: return {IM_COL32(58, 55, 64, 255), IM_COL32(142, 136, 148, 255)};
    case Tier::Copper: return {IM_COL32(107, 58, 34, 255), IM_COL32(217, 130, 75, 255)};
    case Tier::Silver: return {IM_COL32(74, 80, 88, 255), IM_COL32(213, 220, 227, 255)};
    case Tier::Gold: return {IM_COL32(110, 84, 24, 255), IM_COL32(242, 197, 92, 255)};
    case Tier::Ember: return {IM_COL32(122, 30, 14, 255), IM_COL32(255, 135, 56, 255)};
    default: return {Colors().button, Colors().accent};
    }
}

// The icons' order is Icon's; Bracket (9) waits for its event to be announced.
const Award Awards[11] = {
    {"entrant", 0, Tier::Ash, "award.entrant.title", "award.entrant.icon", "award.entrant.how"},
    {"launch-night", 1, Tier::Ash, "award.launch-night.title", "award.launch-night.icon", "award.launch-night.how"},
    {"weekly-top16", 2, Tier::Copper, "award.weekly-top16.title", "award.weekly-top16.icon", "award.weekly-top16.how"},
    {"rollback-regular", 3, Tier::Copper, "award.rollback-regular.title", "award.rollback-regular.icon", "award.rollback-regular.how"},
    {"weekly-top8", 4, Tier::Silver, "award.weekly-top8.title", "award.weekly-top8.icon", "award.weekly-top8.how"},
    {"by-ear", 5, Tier::Silver, "award.by-ear.title", "award.by-ear.icon", "award.by-ear.how"},
    {"monthly-finalist", 6, Tier::Gold, "award.monthly-finalist.title", "award.monthly-finalist.icon", "award.monthly-finalist.how"},
    {"organizer", 7, Tier::Gold, "award.organizer.title", "award.organizer.icon", "award.organizer.how"},
    {"commentary", 8, Tier::Gold, "award.commentary.title", "award.commentary.icon", "award.commentary.how"},
    {"season1-veteran", 10, Tier::Ember, "award.season1-veteran.title", "award.season1-veteran.icon", "award.season1-veteran.how"},
    {"cup-champion", 11, Tier::Ember, "award.cup-champion.title", "award.cup-champion.icon", "award.cup-champion.how"}};
const Award* FindAward(const std::string& id) {
    for (const auto& a : Awards) if (id == a.id) return &a;
    return nullptr;
}
const char* TierName(Tier tier) {
    const char* keys[] = {"", "tier.ash", "tier.copper", "tier.silver", "tier.gold", "tier.ember"};
    return tier == Tier::None ? "" : loc::T(keys[static_cast<int>(tier)]);
}

void Icon(ImDrawList* d, int icon, ImVec2 at, float size, ImU32 ink) {
    // The Titles & icons board's icons by index (the worn icon's number), each
    // named as the catalog's award.*.icon names it.
    static constexpr LineIcon Icons[IconCount] = {LineIcon::Flame /* Spark */, LineIcon::History /* Rewind */, LineIcon::Laurel,
        LineIcon::CalendarCheck, LineIcon::Octagon8, LineIcon::Wave /* Soundwave */, LineIcon::Medal, LineIcon::Flag,
        LineIcon::Microphone, LineIcon::Tournament /* Bracket */, LineIcon::ShieldStar /* Crest */, LineIcon::Crown};
    // Thinner than Tabler's 2: these sit large on their tiles (40 to 56 px).
    if (icon >= 0 && icon < IconCount) DrawLineIcon(d, Icons[icon], at, size, ink, 1.5f);
}

float TitlePlate(ImDrawList* draw, ImVec2 at, float room, float line, const std::string& title, Tier tier) {
    const float s = Scale(), x = at.x, y = at.y;
    // The plate grows to its title, and a title longer than the room steps down a size.
    const float cut = line / 3, stripe = line / 2, need = TextWidth(title, 15 * s, HeadingFont()) + stripe + 10 * s + cut + 6 * s;
    const float w = (std::min)(room, (std::max)(340 * s, need));
    const ImVec2 plate[] = {{x + cut, y}, {x + w, y}, {x + w - cut, y + line}, {x, y + line}};
    const ImVec2 mark[] = {{x + cut, y}, {x + stripe, y}, {x + stripe - cut, y + line}, {x, y + line}};
    const auto colors = TierColor(tier);
    draw->AddConvexPolyFilled(plate, 4, colors.plate);
    draw->AddConvexPolyFilled(mark, 4, colors.stripe);
    const float size = need > w ? 13 * s : 15 * s;
    draw->AddText(HeadingFont(), size, ImVec2(x + stripe + 10 * s, y + (line - size) * .5f), Colors().text, Fit(title, w - stripe - 10 * s - cut, size, HeadingFont()).c_str());
    return w;
}

namespace {
// How far a fact's value sits lower when a key wraps onto a second line: the
// same for every cell, so the rules and values stay level across the cells.
float FactKeyExtra(float width, const std::vector<TagView::Fact>& facts, std::size_t across) {
    const float s = Scale(), room = width / across - 28 * s;
    for (const auto& fact : facts)
        if (HeadingHeight(fact.key, room, 12 * s) > 12 * s) return 12 * s * HeadingLine;
    return 0;
}
std::size_t FactAcross(const std::vector<TagView::Fact>& facts, std::size_t across) { return (std::max<std::size_t>)(1, (std::min)(across, facts.size())); }
}
float FactCellsHeight(float width, const std::vector<TagView::Fact>& facts, std::size_t across) {
    if (facts.empty()) return 0;
    across = FactAcross(facts, across);
    return (52 * Scale() + FactKeyExtra(width, facts, across)) * ((facts.size() + across - 1) / across);
}
float FactCells(ImDrawList* draw, ImVec2 at, float width, const std::vector<TagView::Fact>& facts, std::size_t across) {
    const auto& c = Colors();
    const float s = Scale();
    across = FactAcross(facts, across);
    const float cell = width / across, extra = FactKeyExtra(width, facts, across), height = 52 * s + extra;
    for (std::size_t i = 0; i < facts.size(); ++i) {
        const ImVec2 p(at.x + cell * (i % across), at.y + height * (i / across));
        if (i % across == 0) draw->AddLine(p, ImVec2(at.x + width, p.y), c.border, Hairline());
        else draw->AddLine(p, ImVec2(p.x, p.y + height), c.border, Hairline());
        Heading(draw, ImVec2(p.x + 14 * s, p.y + 10 * s), cell - 28 * s, facts[i].key, 12 * s, c.muted);
        draw->AddText(HeadingFont(), 15 * s, ImVec2(p.x + 14 * s, p.y + 26 * s + extra), c.text, Fit(facts[i].value, cell - 28 * s, 15 * s, HeadingFont()).c_str());
    }
    return FactCellsHeight(width, facts, across);
}

namespace {
// The L tag (Tag): the profile card.
float ProfileCard(ImDrawList* draw, ImVec2 at, float width, const TagView& tag) {
    const auto& c = Colors();
    const float s = Scale(), pad = 20 * s, tile = 96 * s, line = 34 * s, nameSize = 26 * s;
    // The points lines (22 px digits, 20 at 720p) stand under the about line,
    // or under the name and title when there is none, and the band grows to hold them.
    const float digits = (ImGui::GetIO().DisplaySize.y / s <= 720 ? 20 : 22) * s, pointsGap = 4 * s, aboutRow = tag.about.empty() ? 0 : 26 * s;
    const float lines = tag.points.shown ? 2 * digits + pointsGap : 0;
    const float textBottom = 18 * s + line + 8 * s + (tag.title.empty() ? 0 : line + 8 * s) + aboutRow;
    const float band = (std::max)(tile + 36 * s, textBottom + lines + 18 * s);
    const std::size_t count = tag.facts.size(), across = width < 640 * s && count > 2 ? (count + 1) / 2 : count;
    const float height = band + FactCellsHeight(width, tag.facts, across);
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), color::Alpha(c.panel, .94f), 4 * s);
    draw->AddRectFilledMultiColor(at, ImVec2(at.x + width * .7f, at.y + band), c.header, color::Alpha(c.header, 0), color::Alpha(c.header, 0), c.header);
    const ImVec2 t(at.x + pad, at.y + 18 * s);
    if (tag.fighter >= 0) DrawCharacterPortrait(tag.fighter, t, ImVec2(t.x + tile, t.y + tile), c.frame);
    draw->AddRect(t, ImVec2(t.x + tile, t.y + tile), c.accent, 0, 0, 3 * s);
    // The icon worn, in its tier's ink, on a framed tile at the right of the band.
    const float iconTile = tag.icon >= 0 ? 56 * s : 0;
    if (iconTile > 0) {
        const ImVec2 it(at.x + width - pad - iconTile, t.y);
        const auto ink = TierColor(tag.tier).stripe;
        draw->AddRectFilled(it, ImVec2(it.x + iconTile, it.y + iconTile), c.frame, 4 * s);
        draw->AddRect(it, ImVec2(it.x + iconTile, it.y + iconTile), ink, 4 * s, 0, 2 * s);
        Icon(draw, tag.icon, ImVec2(it.x + 8 * s, it.y + 8 * s), iconTile - 16 * s, ink);
    }
    const float x = t.x + tile + 18 * s, room = at.x + width - pad - x - (iconTile > 0 ? iconTile + 12 * s : 0);
    float y = t.y;
    const float marks = DrawNameMarks(draw, x, y + (line - nameSize) * .5f, nameSize, tag.marks).width;
    NoteUserText(tag.name);
    draw->AddText(HeadingFont(), nameSize, ImVec2(x + marks, y + (line - nameSize) * .5f), c.text, Fit(tag.name, room - marks, nameSize, HeadingFont(), false).c_str());
    y += line + 8 * s;
    if (!tag.title.empty()) {
        TitlePlate(draw, ImVec2(x, y), room, line, tag.title, tag.tier);
        y += line + 8 * s;
    }
    if (!tag.about.empty()) draw->AddText(ImGui::GetFont(), 14 * s, ImVec2(x, y), c.muted, Fit(tag.about, room, 14 * s).c_str());
    if (tag.points.shown) LevelLines(draw, ImVec2(x, y + aboutRow), digits, pointsGap, tag.points.fighter, tag.points.account);
    FactCells(draw, ImVec2(at.x, at.y + band), width, tag.facts, across);
    draw->AddRect(at, ImVec2(at.x + width, at.y + height), c.border, 4 * s, 0, Hairline());
    return height;
}
}

float Tag(ImDrawList* draw, ImVec2 at, float width, const TagView& tag, TagSize size) {
    if (size == TagSize::L) return ProfileCard(draw, at, width, tag);
    const float s = Scale(), gap = 8 * s;
    const float font = size == TagSize::XS ? ImGui::GetFontSize() * .85f : ImGui::GetFontSize();
    const float nameSize = size == TagSize::M ? 22 * s : font;
    const float chipHeight = ChipFont() + 8 * s, line = (std::max)(nameSize, chipHeight);
    ImFont* nameFont = size == TagSize::M ? HeadingFont() : ImGui::GetFont();
    const float nameWidth = TextWidth(tag.name, nameSize, nameFont);
    const float marksWidth = NameMarksWidth(tag.marks, nameSize);
    // Indicators sit at the right end: the link and the ping always, the status while there is room.
    // The ping's route goes first: XS, or when the name would not fit beside it.
    const float glyph = LinkGlyph(), right = width - (tag.link ? glyph + gap : 0);
    const bool routed = size != TagSize::XS && !tag.ping.routed.empty() &&
        marksWidth + nameWidth + gap + ChipWidth(tag.ping.routed) <= right;
    const std::string& ping = routed ? tag.ping.routed : tag.ping.text;
    const float pingWidth = ping.empty() ? 0 : ChipWidth(ping);
    const float nameRoom = right - marksWidth - (pingWidth > 0 ? pingWidth + gap : 0);
    // M puts the title and status on a second line; XS and S give them only what the name leaves.
    const bool twoLines = size == TagSize::M && !(tag.title.empty() && tag.status.empty());
    const float titleWidth = tag.title.empty() || twoLines ? 0 : TextWidth(tag.title, font) + gap;
    const float statusWidth = tag.status.empty() ? 0 : ChipWidth(tag.status) + gap;
    const bool status = !tag.status.empty() && (twoLines || nameWidth + titleWidth + statusWidth <= nameRoom);
    const bool title = !tag.title.empty() && (twoLines || nameWidth + titleWidth + (status ? statusWidth : 0) <= nameRoom);
    const float middle = at.y + (line - nameSize) * .5f;
    float x = at.x + DrawNameMarks(draw, at.x, middle, nameSize, tag.marks).width;
    const float nameMax = nameRoom - (!twoLines && title ? titleWidth : 0) - (!twoLines && status ? statusWidth : 0);
    NoteUserText(tag.name);
    const std::string name = Fit(tag.name, (std::max)(0.f, nameMax), nameSize, nameFont, false);
    draw->AddText(nameFont, nameSize, ImVec2(x, middle), Colors().text, name.c_str());
    x += TextWidth(name, nameSize, nameFont) + gap;
    if (!ping.empty()) Chip(draw, ImVec2(at.x + right - pingWidth, at.y + (line - chipHeight) * .5f), ping, tag.ping.color);
    if (tag.link) DrawNetworkLinkGlyph(draw, ImVec2(at.x + width - glyph, at.y + (line - glyph) * .5f), glyph, *tag.link);
    if (!twoLines) {
        if (title) { draw->AddText(ImGui::GetFont(), font, ImVec2(x, at.y + (line - font) * .5f), Colors().muted, tag.title.c_str()); x += titleWidth; }
        if (status) Chip(draw, ImVec2(x, at.y + (line - chipHeight) * .5f), tag.status, Colors().muted);
        return line;
    }
    const float second = at.y + line + 4 * s;
    float x2 = at.x;
    if (title) {
        const std::string fitted = Fit(tag.title, width - (status ? statusWidth : 0), font);
        draw->AddText(ImGui::GetFont(), font, ImVec2(x2, second + (chipHeight - font) * .5f), Colors().muted, fitted.c_str());
        x2 += TextWidth(fitted, font) + gap;
    }
    if (status) Chip(draw, ImVec2(x2, second), tag.status, Colors().muted);
    return line + 4 * s + chipHeight;
}
float TagWidth(const TagView& tag, TagSize size) {
    const float s = Scale(), gap = 8 * s;
    const float font = size == TagSize::XS ? ImGui::GetFontSize() * .85f : ImGui::GetFontSize();
    // As Tag lays the line out: the ping with its route above XS.
    const std::string& ping = size != TagSize::XS && !tag.ping.routed.empty() ? tag.ping.routed : tag.ping.text;
    // Rounded up to a whole pixel: Tag takes the other parts from the width in
    // its own order, and the float error must not leave the name a hair short
    // of its measure, which would cut it ("FRac...").
    return std::ceil(NameMarksWidth(tag.marks, font) + TextWidth(tag.name, font, ImGui::GetFont()) + (tag.title.empty() ? 0 : gap + TextWidth(tag.title, font)) +
        (tag.status.empty() ? 0 : gap + ChipWidth(tag.status)) + (ping.empty() ? 0 : gap + ChipWidth(ping)) +
        (tag.link ? gap + LinkGlyph() : 0));
}
} } }
