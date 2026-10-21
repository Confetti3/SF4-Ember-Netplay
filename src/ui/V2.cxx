#include "V2.hxx"
#include "Theme.hxx"
#include <cfloat>

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

void Focus(ImDrawList* draw, ImVec2 min, ImVec2 max, bool bar) {
    const float s = Scale(), r = 4 * s;
    draw->AddRect(ImVec2(min.x + s, min.y + s), ImVec2(max.x - s, max.y - s), palette::Ember, r, 0, 2 * s);
    if (bar) draw->AddRectFilled(min, ImVec2(min.x + 4 * s, max.y), palette::Ember, r, ImDrawFlags_RoundCornersLeft);
}

float ChipWidth(const std::string& text) { return TextWidth(text, ChipFont()) + 16 * Scale(); }
float Chip(ImDrawList* draw, ImVec2 at, const std::string& text, ImU32 tone) {
    const float s = Scale(), size = ChipFont(), width = ChipWidth(text);
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + size + 8 * s), color::Alpha(tone, .18f), 4 * s);
    draw->AddText(ImGui::GetFont(), size, ImVec2(at.x + 8 * s, at.y + 4 * s), tone, text.c_str());
    return width;
}

void Header(const char* title, const char* eyebrow) {
    const float s = Scale();
    const auto p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool narrow = Narrow(ImGui::GetWindowWidth(), s);
    auto* draw = ImGui::GetWindowDrawList();
    auto* atlas = ImGui::GetIO().Fonts;
    ImVec2 uv0, uv1;
    atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(0), &uv0, &uv1);
    const float mark = (narrow ? 32 : 40) * s, gap = 12 * s, x = p.x + mark + gap;
    const float eyebrowSize = 13 * s, titleSize = (narrow ? 24 : 28) * s;
    const float eyebrowHeight = eyebrow ? eyebrowSize + 4 * s : 0;
    const float height = (std::max)(mark, eyebrowHeight + titleSize) + 10 * s;
    draw->AddImage(atlas->TexID, ImVec2(p.x, p.y + (height - 10 * s - mark) * .5f), ImVec2(p.x + mark, p.y + (height - 10 * s + mark) * .5f), uv0, uv1);
    float y = p.y + (height - 10 * s - eyebrowHeight - titleSize) * .5f;
    if (eyebrow) {
        draw->AddText(ImGui::GetFont(), eyebrowSize, ImVec2(x, y), palette::Muted, Fit(eyebrow, width - mark - gap, eyebrowSize).c_str());
        y += eyebrowHeight;
    }
    draw->AddText(HeadingFont(), titleSize, ImVec2(x, y), palette::Ivory, Fit(title, width - mark - gap, titleSize, HeadingFont()).c_str());
    draw->AddLine(ImVec2(p.x, p.y + height), ImVec2(p.x + width, p.y + height), color::Border, s);
    draw->AddRectFilled(ImVec2(p.x, p.y + height - s), ImVec2(p.x + 72 * s, p.y + height + 2 * s), palette::Ember);
    ImGui::Dummy(ImVec2(width, height + 8 * s));
}

int Tabs(const std::vector<std::string>& labels, int current) {
    const float s = Scale(), height = 44 * s, pad = 16 * s;
    const auto start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    auto* draw = ImGui::GetWindowDrawList();
    int pressed = -1;
    float x = start.x;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const float w = ImGui::CalcTextSize(labels[i].c_str()).x + 2 * pad;
        ImGui::SetCursorScreenPos(ImVec2(x, start.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(w, height))) pressed = i;
        ImGui::PopID();
        const bool selected = i == current;
        if (selected) {
            draw->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + height), color::Header);
            draw->AddRectFilled(ImVec2(x, start.y + height - 3 * s), ImVec2(x + w, start.y + height), palette::Ember);
        } else if (ImGui::IsItemHovered()) draw->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + height), color::Hover);
        draw->AddText(ImVec2(x + pad, start.y + (height - ImGui::GetFontSize()) * .5f), selected ? palette::Ivory : palette::Muted, labels[i].c_str());
        x += w;
    }
    draw->AddLine(ImVec2(start.x, start.y + height), ImVec2(start.x + width, start.y + height), color::Border, s);
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(width, height + 8 * s));
    return pressed;
}

float Tag(ImDrawList* draw, ImVec2 at, float width, const TagView& tag, TagSize size) {
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
    draw->AddText(nameFont, nameSize, ImVec2(x, middle), palette::Ivory, name.c_str());
    x += TextWidth(name, nameSize, nameFont) + gap;
    if (!ping.empty()) Chip(draw, ImVec2(at.x + width - pingWidth, at.y + (line - chipHeight) * .5f), ping, tag.ping.color);
    if (!twoLines) {
        if (title) { draw->AddText(ImGui::GetFont(), font, ImVec2(x, at.y + (line - font) * .5f), palette::Muted, tag.title.c_str()); x += titleWidth; }
        if (status) Chip(draw, ImVec2(x, at.y + (line - chipHeight) * .5f), tag.status, palette::Muted);
        return line;
    }
    const float second = at.y + line + 4 * s;
    float x2 = at.x;
    if (title) {
        const std::string fitted = Fit(tag.title, width - (status ? statusWidth : 0), font);
        draw->AddText(ImGui::GetFont(), font, ImVec2(x2, second + (chipHeight - font) * .5f), palette::Muted, fitted.c_str());
        x2 += TextWidth(fitted, font) + gap;
    }
    if (status) Chip(draw, ImVec2(x2, second), tag.status, palette::Muted);
    return line + 4 * s + chipHeight;
}
} } }
