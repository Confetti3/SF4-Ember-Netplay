#include "V2.hxx"
#include "Theme.hxx"
#include "GameMenu.hxx"
#include "MenuGlyphs.hxx"
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>

namespace sf4e { namespace ui { namespace v2 {
namespace {
bool on = false;
float ChipFont() { return ImGui::GetFontSize() * .78f; }
// `text` cut with "..." to `width` at `size`.
std::string Fit(const std::string& text, float width, float size, ImFont* font = ImGui::GetFont()) {
    if (font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x <= width) return text;
    const char* end = nullptr;
    font->CalcTextSizeA(size, (std::max)(0.f, width - font->CalcTextSizeA(size, FLT_MAX, 0, "...").x), 0, text.c_str(), nullptr, &end);
    return std::string(text.c_str(), end) + "...";
}
float TextWidth(const std::string& text, float size, ImFont* font = ImGui::GetFont()) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; }
}

bool On() { return on; }
void SetOn(bool value) { on = value; }

namespace {
Brand brand = Brand::Ember;
constexpr Palette EmberPalette{IM_COL32(20, 19, 18, 255), IM_COL32(25, 23, 21, 255), IM_COL32(33, 30, 27, 255), IM_COL32(56, 45, 36, 255),
    IM_COL32(58, 42, 31, 255), IM_COL32(89, 64, 43, 255), IM_COL32(68, 58, 49, 255), IM_COL32(255, 135, 56, 255), IM_COL32(255, 177, 111, 255),
    IM_COL32(243, 235, 221, 255), IM_COL32(181, 169, 155, 255), IM_COL32(255, 92, 80, 255)};
// BluMint's: near-black navy grounds and its brand blue, as blumint.com.
constexpr Palette BluMintPalette{IM_COL32(5, 9, 13, 255), IM_COL32(10, 20, 28, 255), IM_COL32(12, 24, 34, 255), IM_COL32(16, 34, 48, 255),
    IM_COL32(10, 46, 68, 255), IM_COL32(18, 58, 82, 255), IM_COL32(30, 58, 78, 255), IM_COL32(33, 187, 255, 255), IM_COL32(127, 215, 255, 255),
    IM_COL32(236, 244, 250, 255), IM_COL32(140, 164, 182, 255), IM_COL32(255, 77, 77, 255)};
}
void SetBrand(Brand value) { brand = value; }
Brand CurrentBrand() { return brand; }
const Palette& Colors() { return brand == Brand::BluMint ? BluMintPalette : EmberPalette; }

void Focus(ImDrawList* draw, ImVec2 min, ImVec2 max, bool bar) {
    const float s = Scale(), r = 4 * s;
    draw->AddRect(ImVec2(min.x + s, min.y + s), ImVec2(max.x - s, max.y - s), Colors().accent, r, 0, 2 * s);
    if (bar) draw->AddRectFilled(min, ImVec2(min.x + 4 * s, max.y), Colors().accent, r, ImDrawFlags_RoundCornersLeft);
}

void CardGround(ImDrawList* draw, Box card, bool focused, bool hovered, bool act) {
    const float s = Scale();
    draw->AddRectFilled(card.min, card.max, focused ? Colors().header : hovered ? Colors().hover : color::Alpha(Colors().frame, .92f), 4 * s);
    draw->AddRect(card.min, card.max, color::Alpha(Colors().border, .6f), 4 * s, 0, s);
    if (act) draw->AddRectFilled(card.min, ImVec2(card.min.x + 3 * s, card.max.y), Colors().accent, 4 * s, ImDrawFlags_RoundCornersLeft);
}

int HintBar(Box bar, const std::vector<Hint>& hints, const std::string& status, ImU32 tone, bool bell) {
    const float s = Scale(), glyph = (bar.H() < 52 * s ? 26 : 30) * s, font = 16 * s;
    auto* d = ImGui::GetWindowDrawList();
    const float side = SideMargin(ImGui::GetWindowWidth(), s);
    d->AddRectFilled(bar.min, bar.max, color::Alpha(Colors().ink, .92f));
    d->AddLine(bar.min, ImVec2(bar.max.x, bar.min.y), Colors().border, s);
    int pressed = -1;
    float x = bar.min.x + side;
    for (int i = 0; i < static_cast<int>(hints.size()); ++i) {
        const auto& h = hints[i];
        const std::string label = PromptLabel(h.glyph, h.label, MenuPromptBrand());
        const float w = glyph + 8 * s + ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0, label.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(x, bar.min.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##hint", ImVec2(w, bar.H()))) pressed = i;
        ImGui::PopID();
        DrawPromptGlyph(d, h.glyph, ImVec2(x, bar.min.y + (bar.H() - glyph) * .5f), glyph, MenuArt(), s);
        d->AddText(ImGui::GetFont(), font, ImVec2(x + glyph + 8 * s, bar.min.y + (bar.H() - font) * .5f), Colors().text, label.c_str());
        x += w + 24 * s;
    }
    const float right = bar.max.x - side, size = 15 * s, icon = bell ? 30 * s : 0;
    const std::string fitted = Fit(status, (std::max)(0.f, right - x - icon), size);
    const float left = right - TextWidth(fitted, size) - icon, mid = bar.min.y + bar.H() * .5f;
    d->AddText(ImGui::GetFont(), size, ImVec2(right - TextWidth(fitted, size), mid - size * .5f), tone, fitted.c_str());
    if (bell) {
        // The bell of the Toasts board's touch control: a dome on a rim, and its clapper.
        ImGui::SetCursorScreenPos(ImVec2(left, bar.min.y));
        if (ImGui::InvisibleButton("##hint-bell", ImVec2(right - left, bar.H()))) pressed = static_cast<int>(hints.size());
        const ImU32 ink = ImGui::IsItemHovered() ? Colors().accent : Colors().text;
        const float k = s * .8f, bx = left, by = mid - 11 * k;
        d->PathLineTo(ImVec2(bx + 6 * k, by + 15 * k));
        d->PathLineTo(ImVec2(bx + 6 * k, by + 10 * k));
        d->PathArcTo(ImVec2(bx + 11 * k, by + 10 * k), 5 * k, 3.14159265f, 6.28318531f);
        d->PathLineTo(ImVec2(bx + 16 * k, by + 15 * k));
        d->PathLineTo(ImVec2(bx + 18 * k, by + 17 * k));
        d->PathLineTo(ImVec2(bx + 4 * k, by + 17 * k));
        d->PathStroke(ink, ImDrawFlags_Closed, 2 * k);
        d->AddLine(ImVec2(bx + 9 * k, by + 20 * k), ImVec2(bx + 13 * k, by + 20 * k), ink, 2 * k);
    }
    return pressed;
}

int SpotMap(const char* id, const std::vector<ImVec2>& spots, int current, ImVec2 block) {
    const float s = Scale(), w = (std::min)(300 * s, ImGui::GetContentRegionAvail().x), h = w * 9 / 16;
    const auto p = ImGui::GetCursorScreenPos();
    auto* d = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool held = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Colors().frame, 4 * s);
    d->AddRect(p, ImVec2(p.x + w, p.y + h), hovered || held ? Colors().accent : Colors().border, 4 * s, 0, s);
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
        d->AddRect(q, ImVec2(q.x + size.x, q.y + size.y), held && i == nearest ? Colors().accent : color::Alpha(Colors().muted, .5f), 2 * s, 0, s);
    }
    const ImVec2 q = held ? ImVec2(mouse.x - size.x * .5f, mouse.y - size.y * .5f) : at(spots[(std::max)(0, (std::min)(static_cast<int>(spots.size()) - 1, current))]);
    d->AddRectFilled(q, ImVec2(q.x + size.x, q.y + size.y), Colors().accent, 2 * s);
    return ImGui::IsItemDeactivated() && hovered ? nearest : -1;
}

float Grip(const char* id, ImVec2 from, ImVec2 to) {
    const float s = Scale(), reach = 22 * s;
    const bool down = from.x == to.x;
    const ImVec2 mid((from.x + to.x) * .5f, (from.y + to.y) * .5f);
    ImGui::SetCursorScreenPos(down ? ImVec2(from.x - reach, from.y) : ImVec2(from.x, from.y - reach));
    ImGui::InvisibleButton(id, down ? ImVec2(2 * reach, (std::max)(1.f, to.y - from.y)) : ImVec2((std::max)(1.f, to.x - from.x), 2 * reach));
    const bool held = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    if (held || hovered) ImGui::SetMouseCursor(down ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    auto* d = ImGui::GetWindowDrawList();
    const auto& c = Colors();
    const ImU32 line = held ? c.accent : hovered ? color::Alpha(c.accent, .7f) : color::Alpha(c.accent, .35f);
    d->AddLine(from, to, line, 4 * s);
    d->AddCircleFilled(mid, 18 * s, held ? c.accent : c.button);
    d->AddCircle(mid, 18 * s, line, 0, s);
    // Three dots at rest; held, the two ways it moves.
    const ImU32 ink = held ? c.ink : c.text;
    if (held) for (float side : {-1.f, 1.f}) {
        const ImVec2 tip = down ? ImVec2(mid.x + side * 10 * s, mid.y) : ImVec2(mid.x, mid.y + side * 10 * s);
        const ImVec2 a = down ? ImVec2(tip.x - side * 6 * s, mid.y - 6 * s) : ImVec2(mid.x - 6 * s, tip.y - side * 6 * s);
        const ImVec2 b = down ? ImVec2(tip.x - side * 6 * s, mid.y + 6 * s) : ImVec2(mid.x + 6 * s, tip.y - side * 6 * s);
        d->AddTriangleFilled(tip, a, b, ink);
    }
    else for (int i = -1; i <= 1; ++i) d->AddCircleFilled(down ? ImVec2(mid.x, mid.y + i * 6 * s) : ImVec2(mid.x + i * 6 * s, mid.y), 2 * s, ink);
    return held ? (down ? ImGui::GetMousePos().x : ImGui::GetMousePos().y) : NAN;
}

ImVec4 BoardGround(bool seeThrough) {
    return on ? ImGui::ColorConvertU32ToFloat4(color::Alpha(Colors().panel, seeThrough ? .6f : .84f)) : BoardPanelColor(seeThrough);
}

float ChipWidth(const std::string& text) { return TextWidth(text, ChipFont()) + 16 * Scale(); }
float Chip(ImDrawList* draw, ImVec2 at, const std::string& text, ImU32 tone) {
    const float s = Scale(), size = ChipFont(), width = ChipWidth(text);
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + size + 8 * s), color::Alpha(tone, .18f), 4 * s);
    draw->AddText(ImGui::GetFont(), size, ImVec2(at.x + 8 * s, at.y + 4 * s), tone, text.c_str());
    return width;
}

void Paragraph(const std::string& text, float size, ImU32 color, ImFont* font) {
    const float width = ImGui::GetContentRegionAvail().x, line = size * 1.45f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float y = start.y;
    // ImGui's wrap runs on through a newline, so each line is wrapped up to its own.
    for (const char *p = text.c_str(), *end = p + text.size(); p < end; y += line) {
        const char* stop = std::find(p, end, '\n');
        const char* cut = p == stop ? stop : font->CalcWordWrapPositionA(size / font->FontSize, p, stop, width);
        if (cut == p && p != stop) ++cut;  // ponytail: a pane narrower than one glyph splits it
        ImGui::GetWindowDrawList()->AddText(font, size, ImVec2(start.x, y + (line - size) * .5f), color, p, cut);
        for (p = cut; p < stop && *p == ' '; ++p) {}
        if (p == stop && p < end) ++p;
    }
    ImGui::Dummy(ImVec2(width, y - start.y));
}
void Wrapped(const std::string& text) {
    if (On()) Paragraph(text, ImGui::GetFontSize(), ImGui::GetColorU32(ImGuiCol_Text));
    else ImGui::TextWrapped("%s", text.c_str());
}
void Fact(const std::string& key, const std::string& value) {
    const float s = Scale(), w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() + 20 * s, room = w - 24 * s;
    const auto p = ImGui::GetCursorScreenPos();
    auto* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Colors().frame, 4 * s);
    d->AddRect(p, ImVec2(p.x + w, p.y + h), Colors().border, 4 * s, 0, s);
    const std::string shown = Fit(value, room * .6f, ImGui::GetFontSize());
    const float vw = TextWidth(shown, ImGui::GetFontSize());
    d->AddText(ImVec2(p.x + 12 * s, p.y + 10 * s), Colors().muted, Fit(key, room - vw - 16 * s, ImGui::GetFontSize()).c_str());
    d->AddText(ImVec2(p.x + w - 12 * s - vw, p.y + 10 * s), Colors().text, shown.c_str());
    ImGui::Dummy(ImVec2(w, h));
}

float BrandMark(ImDrawList* draw, ImVec2 at, float height) {
    auto* atlas = ImGui::GetIO().Fonts;
    ImVec2 uv0, uv1;
    if (brand == Brand::Ember) {
        atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(0), &uv0, &uv1);
        draw->AddImage(atlas->TexID, at, ImVec2(at.x + height, at.y + height), uv0, uv1);
        return height;
    }
    // BluMint's wordmark, then a small × and Ember's mark: BluMint's match, played in Ember.
    const float s = Scale(), w = height * 4 * .7f, h = height * .7f, mark = height * .6f;
    atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(WordmarkRect), &uv0, &uv1);
    draw->AddImage(atlas->TexID, ImVec2(at.x, at.y + (height - h) * .5f), ImVec2(at.x + w, at.y + (height + h) * .5f), uv0, uv1);
    float x = at.x + w + 10 * s;
    draw->AddText(ImGui::GetFont(), 14 * s, ImVec2(x, at.y + (height - 14 * s) * .5f), Colors().muted, "x");
    x += 18 * s;
    atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(0), &uv0, &uv1);
    draw->AddImage(atlas->TexID, ImVec2(x, at.y + (height - mark) * .5f), ImVec2(x + mark, at.y + (height + mark) * .5f), uv0, uv1);
    return x + mark - at.x;
}

bool Header(const char* title, const char* eyebrow, const std::vector<HeaderChip>& chips, const char* back) {
    const float s = Scale();
    const auto p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool narrow = Narrow(ImGui::GetWindowWidth(), s);
    auto* draw = ImGui::GetWindowDrawList();
    const float markHeight = (narrow ? 32 : 40) * s, gap = 16 * s;
    const float eyebrowSize = 13 * s, titleSize = (narrow ? 22 : 26) * s;
    const float eyebrowHeight = eyebrow ? eyebrowSize + 4 * s : 0;
    const float height = (std::max)(markHeight, eyebrowHeight + titleSize) + 10 * s;
    // Back: a chevron and its word on a button, as the boards draw it.
    float left = p.x;
    bool pressed = false;
    if (back) {
        const float bh = 44 * s, bw = 34 * s + TextWidth(back, 16 * s) + 12 * s;
        const ImVec2 at(p.x, p.y + (height - 10 * s - bh) * .5f), to(at.x + bw, at.y + bh);
        ImGui::SetCursorScreenPos(at);
        pressed = ImGui::InvisibleButton("##header-back", ImVec2(bw, bh));
        draw->AddRectFilled(at, to, ImGui::IsItemHovered() ? Colors().hover : Colors().button, 4 * s);
        draw->AddRect(at, to, color::Alpha(Colors().accent, .35f), 4 * s, 0, s);
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
        draw->AddText(ImGui::GetFont(), eyebrowSize, ImVec2(x, y), Colors().muted, Fit(eyebrow, room, eyebrowSize).c_str());
        y += eyebrowHeight;
    }
    draw->AddText(HeadingFont(), titleSize, ImVec2(x, y), Colors().text, Fit(title, room, titleSize, HeadingFont()).c_str());
    draw->AddLine(ImVec2(p.x, p.y + height), ImVec2(p.x + width, p.y + height), Colors().border, s);
    draw->AddRectFilled(ImVec2(x, p.y + height - s), ImVec2(x + 72 * s, p.y + height + 2 * s), Colors().accent);
    ImGui::Dummy(ImVec2(width, height + 8 * s));
    return pressed;
}

int Tabs(const std::vector<std::string>& labels, int current) {
    const float s = Scale(), height = 44 * s, pad = 16 * s;
    const auto start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    auto* draw = ImGui::GetWindowDrawList();
    int pressed = -1;
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
    draw->AddLine(ImVec2(start.x, start.y + height), ImVec2(start.x + width, start.y + height), Colors().border, s);
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(width, height + 8 * s));
    return pressed;
}

namespace {
// The icons' SVG paths (the Titles & icons board): M, L, H, V, C, A and Z,
// absolute or relative. Each is stroked at 3.5 on its 64 grid.
const char* const IconPaths[IconCount] = {
    "M32 6c4 10 15 15 15 29a15 15 0 0 1-30 0c0-8 4-12 7-16 0 6 3 10 7 10-2-9 1-16 1-23z M32 46a5 5 0 0 0 5-5c0-4-3-6-5-9-2 3-5 5-5 9a5 5 0 0 0 5 5z",  // Spark
    "M50 32a18 18 0 1 1-6-13.4 M46 8v12H34",  // Rewind
    "M20 54C9 45 7 30 14 14M44 54c11-9 13-24 6-40 M14 24l6 2M12 34h7M15 44l6-3M50 24l-6 2M52 34h-7M49 44l-6-3",  // Laurel
    "M10 14h44v40H10zM10 24h44 M20 8v10M44 8v10M24 38l6 6 12-12",  // Calendar
    "M22 6h20l14 14v24L42 58H22L8 44V20z M25 25a7 7 0 1 0 14 0a7 7 0 1 0-14 0M24 40a8 8 0 1 0 16 0a8 8 0 1 0-16 0",  // Octagon 8
    "M10 26v12M20 18v28M30 10v44M40 20v24M50 28v8 M56 30v4",  // Soundwave
    "M20 6l12 20 12-20 M18 40a14 14 0 1 0 28 0a14 14 0 1 0-28 0",  // Medal
    "M14 58V8 M14 10h34l-8 10 8 10H14",  // Flag
    "M24 12a8 8 0 0 1 16 0v18a8 8 0 0 1-16 0z M16 28a16 16 0 0 0 32 0M32 44v12M22 56h20",  // Mic
    "M8 14h14v36H8M22 32h14 M36 20h12v24H36M48 32h8",  // Bracket
    "M32 6l22 8v16c0 14-10 24-22 28C20 54 10 44 10 30V14z M32 20l4 9 9 1-7 6 2 9-8-5-8 5 2-9-7-6 9-1z",  // Crest
    "M10 46h44l4-26-14 10-12-18-12 18-14-10z M10 54h44",  // Crown
};
// An SVG arc from `from` to `to` as ImGui's arc about its centre (SVG 1.1, F.6.5).
void ArcTo(ImDrawList* d, ImVec2 from, float rx, float ry, bool large, bool sweep, ImVec2 to, float k) {
    const float hx = (from.x - to.x) * .5f, hy = (from.y - to.y) * .5f;
    const float scaleUp = (std::max)(1.f, std::sqrt(hx * hx / (rx * rx) + hy * hy / (ry * ry)));
    rx *= scaleUp; ry *= scaleUp;
    const float num = rx * rx * ry * ry - rx * rx * hy * hy - ry * ry * hx * hx, den = rx * rx * hy * hy + ry * ry * hx * hx;
    const float root = (large == sweep ? -1.f : 1.f) * std::sqrt((std::max)(0.f, num / den));
    const float cx = root * rx * hy / ry + (from.x + to.x) * .5f, cy = -root * ry * hx / rx + (from.y + to.y) * .5f;
    const float a0 = std::atan2((from.y - cy) / ry, (from.x - cx) / rx);
    float a1 = std::atan2((to.y - cy) / ry, (to.x - cx) / rx);
    if (sweep && a1 < a0) a1 += 2 * 3.14159265f;
    if (!sweep && a1 > a0) a1 -= 2 * 3.14159265f;
    // ponytail: circles only (rx == ry) in these icons; an ellipse would need its own segments.
    d->PathArcTo(ImVec2(cx, cy), rx, a0, a1, static_cast<int>(8 + rx * k));
}
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

void Icon(ImDrawList* d, int icon, ImVec2 at, float size, ImU32 ink) {
    if (icon < 0 || icon >= IconCount) return;
    const float k = size / 64;
    const auto P = [&](float x, float y) { return ImVec2(at.x + x * k, at.y + y * k); };
    const char* c = IconPaths[icon];
    char command = 0;
    float x = 0, y = 0, sx = 0, sy = 0;
    const auto stroke = [&](bool closed) { d->PathStroke(ink, closed ? ImDrawFlags_Closed : 0, 3.5f * k); };
    const auto number = [&] { char* end; const float v = std::strtof(c, &end); c = end; while (*c == ' ' || *c == ',') ++c; return v; };
    while (*c) {
        if (std::isalpha(static_cast<unsigned char>(*c))) { command = *c++; while (*c == ' ') ++c; }
        const bool rel = std::islower(static_cast<unsigned char>(command));
        const float ox = rel ? x : 0, oy = rel ? y : 0;
        switch (std::toupper(static_cast<unsigned char>(command))) {
        case 'M':
            if (!d->_Path.empty()) stroke(false);
            x = ox + number(); y = oy + number(); sx = x; sy = y;
            d->PathLineTo(P(x, y));
            command = rel ? 'l' : 'L';  // further pairs are lines
            break;
        case 'L': x = ox + number(); y = oy + number(); d->PathLineTo(P(x, y)); break;
        case 'H': x = ox + number(); d->PathLineTo(P(x, y)); break;
        case 'V': y = oy + number(); d->PathLineTo(P(x, y)); break;
        case 'C': {
            const float x1 = ox + number(), y1 = oy + number(), x2 = ox + number(), y2 = oy + number();
            x = ox + number(); y = oy + number();
            d->PathBezierCubicCurveTo(P(x1, y1), P(x2, y2), P(x, y));
            break;
        }
        case 'A': {
            const float rx = number(), ry = number();
            number();
            const bool large = number() != 0, sweep = number() != 0;
            const ImVec2 from(x, y);
            x = ox + number(); y = oy + number();
            ArcTo(d, P(from.x, from.y), rx * k, ry * k, large, sweep, P(x, y), 1);
            break;
        }
        case 'Z': x = sx; y = sy; stroke(true); break;
        default: ++c; break;
        }
    }
    if (!d->_Path.empty()) stroke(false);
}

namespace {
// The L tag (Tag): the profile card.
float ProfileCard(ImDrawList* draw, ImVec2 at, float width, const TagView& tag) {
    const auto& c = Colors();
    const float s = Scale(), pad = 20 * s, tile = 96 * s, band = tile + 36 * s, line = 34 * s, nameSize = 26 * s;
    const std::size_t count = tag.facts.size(), across = width < 640 * s && count > 2 ? (count + 1) / 2 : count;
    const float factHeight = 52 * s, height = band + (count ? factHeight * ((count + across - 1) / across) : 0);
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
    draw->AddText(HeadingFont(), nameSize, ImVec2(x + marks, y + (line - nameSize) * .5f), c.text, Fit(tag.name, room - marks, nameSize, HeadingFont()).c_str());
    y += line + 8 * s;
    // The title on its plate: a slanted bar with the accent's stripe at its start.
    if (!tag.title.empty()) {
        // The plate grows to its title, and a title longer than the room steps down a size.
        const float cut = line / 3, stripe = line / 2, need = TextWidth(tag.title, 15 * s, HeadingFont()) + stripe + 10 * s + cut + 6 * s;
        const float w = (std::min)(room, (std::max)(340 * s, need));
        const ImVec2 plate[] = {{x + cut, y}, {x + w, y}, {x + w - cut, y + line}, {x, y + line}};
        const ImVec2 mark[] = {{x + cut, y}, {x + stripe, y}, {x + stripe - cut, y + line}, {x, y + line}};
        const auto tier = TierColor(tag.tier);
        draw->AddConvexPolyFilled(plate, 4, tier.plate);
        draw->AddConvexPolyFilled(mark, 4, tier.stripe);
        const float size = need > w ? 13 * s : 15 * s;
        draw->AddText(HeadingFont(), size, ImVec2(x + stripe + 10 * s, y + (line - size) * .5f), c.text, Fit(tag.title, w - stripe - 10 * s - cut, size, HeadingFont()).c_str());
        y += line + 8 * s;
    }
    draw->AddText(ImGui::GetFont(), 14 * s, ImVec2(x, y), c.muted, Fit(tag.about, room, 14 * s).c_str());
    // The facts, a cell each, key over value.
    const float cell = count ? width / across : 0;
    for (std::size_t i = 0; i < count; ++i) {
        const ImVec2 p(at.x + cell * (i % across), at.y + band + factHeight * (i / across));
        if (i % across == 0) draw->AddLine(p, ImVec2(at.x + width, p.y), c.border, s);
        else draw->AddLine(p, ImVec2(p.x, p.y + factHeight), c.border, s);
        draw->AddText(HeadingFont(), 12 * s, ImVec2(p.x + 14 * s, p.y + 10 * s), c.muted, Fit(tag.facts[i].key, cell - 28 * s, 12 * s, HeadingFont()).c_str());
        draw->AddText(HeadingFont(), 15 * s, ImVec2(p.x + 14 * s, p.y + 26 * s), c.text, Fit(tag.facts[i].value, cell - 28 * s, 15 * s, HeadingFont()).c_str());
    }
    draw->AddRect(at, ImVec2(at.x + width, at.y + height), c.border, 4 * s, 0, s);
    return height;
}
}

float Tag(ImDrawList* draw, ImVec2 at, float width, const TagView& tag, TagSize size) {
    if (size == TagSize::L) return ProfileCard(draw, at, width, tag);
    const float s = Scale(), gap = 8 * s;
    const float font = size == TagSize::XS ? ImGui::GetFontSize() * .85f : ImGui::GetFontSize();
    const float nameSize = size == TagSize::M ? 22 * s : font;
    const float chipHeight = ChipFont() + 8 * s, line = (std::max)(nameSize, chipHeight);
    // Indicators sit at the right end: the ping always, the status while there is room.
    const std::string& ping = tag.ping.text;
    const float pingWidth = ping.empty() ? 0 : ChipWidth(ping);
    const float marksWidth = NameMarksWidth(tag.marks, nameSize);
    const float nameRoom = width - marksWidth - (pingWidth > 0 ? pingWidth + gap : 0);
    // M puts the title and status on a second line; XS and S give them only what the name leaves.
    const bool twoLines = size == TagSize::M && !(tag.title.empty() && tag.status.empty());
    ImFont* nameFont = size == TagSize::M ? HeadingFont() : ImGui::GetFont();
    const float nameWidth = TextWidth(tag.name, nameSize, nameFont);
    const float titleWidth = tag.title.empty() || twoLines ? 0 : TextWidth(tag.title, font) + gap;
    const float statusWidth = tag.status.empty() ? 0 : ChipWidth(tag.status) + gap;
    const bool status = !tag.status.empty() && (twoLines || nameWidth + titleWidth + statusWidth <= nameRoom);
    const bool title = !tag.title.empty() && (twoLines || nameWidth + titleWidth + (status ? statusWidth : 0) <= nameRoom);
    const float middle = at.y + (line - nameSize) * .5f;
    float x = at.x + DrawNameMarks(draw, at.x, middle, nameSize, tag.marks).width;
    const float nameMax = nameRoom - (!twoLines && title ? titleWidth : 0) - (!twoLines && status ? statusWidth : 0);
    NoteUserText(tag.name);
    const std::string name = Fit(tag.name, (std::max)(0.f, nameMax), nameSize, nameFont);
    draw->AddText(nameFont, nameSize, ImVec2(x, middle), Colors().text, name.c_str());
    x += TextWidth(name, nameSize, nameFont) + gap;
    if (!ping.empty()) Chip(draw, ImVec2(at.x + width - pingWidth, at.y + (line - chipHeight) * .5f), ping, tag.ping.color);
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
} } }
