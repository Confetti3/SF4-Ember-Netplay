// The V2 look of a menu screen (the V2 Overhaul design): a brand or screen
// header, optional tabs, the entries as cards, a detail pane when there is
// room, and the hint bar along the bottom. Input, navigation and the dialogs
// are GameMenu's own, so a screen behaves the same in either look.
#include "GameMenu.hxx"
#include "MenuGlyphs.hxx"
#include "MenuPresentation.hxx"
#include "SelectionArt.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cstring>

namespace sf4e { namespace ui {
namespace {
// `text` cut with "..." to `width`.
std::string Fit(const std::string& text, float width, float size, ImFont* font) {
    if (font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x <= width) return text;
    const char* end = nullptr;
    font->CalcTextSizeA(size, (std::max)(0.f, width - font->CalcTextSizeA(size, FLT_MAX, 0, "...").x), 0, text.c_str(), nullptr, &end);
    return std::string(text.c_str(), end) + "...";
}
// A title written in capitals, as the classic headers are.
bool IsCapitals(const char* text) {
    bool letter = false;
    for (; *text; ++text) { if (*text >= 'a' && *text <= 'z') return false; letter |= *text >= 'A' && *text <= 'Z'; }
    return letter;
}
// The first line of a detail, for a card's second line.
std::string FirstLine(const std::string& text) { return text.substr(0, text.find('\n')); }
void Chevron(ImDrawList* d, ImVec2 at, float size, ImU32 color) {
    d->AddPolyline(std::array<ImVec2, 3>{ImVec2(at.x, at.y - size), ImVec2(at.x + size * .6f, at.y), ImVec2(at.x, at.y + size)}.data(), 3, color, 0, 2 * Scale());
}
// Home's brand block: the emblem, EMBER and the subtitle. Returns its height.
float Brand(ImDrawList* d, ImVec2 p, bool compact) {
    const float s = Scale(), mark = (compact ? 44 : 72) * s, size = (compact ? 34 : 56) * s;
    auto* atlas = ImGui::GetIO().Fonts;
    ImVec2 uv0, uv1;
    atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(0), &uv0, &uv1);
    d->AddImage(atlas->TexID, p, ImVec2(p.x + mark, p.y + mark), uv0, uv1);
    const float x = p.x + mark + 16 * s;
    d->AddText(HeadingFont(), size, ImVec2(x, p.y + (mark - size - 18 * s) * .5f), palette::Ivory, "EMBER");
    d->AddText(HeadingFont(), 13 * s, ImVec2(x + 2 * s, p.y + (mark + size - 18 * s) * .5f + 4 * s), palette::Ember, loc::T("brand.subtitle"));
    return mark;
}
}

MenuAction GameMenu::DrawV2(const char* title, const std::vector<MenuEntry>& entries, const char* status, const Detail& detail, int columns,
    const Card& card, const Body& body, float cardHeight, Tone statusTone, bool home, MenuAction action, unsigned held,
    bool modalAtStart, bool choosingAtStart, bool acceptEditText, bool noticeOpen) {
    const float s = Scale();
    auto* d = ImGui::GetWindowDrawList();
    const auto win = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const bool narrow = v2::Narrow(size.x, s), changed = lastScreen_ != navigation.Screen();
    const float side = v2::SideMargin(size.x, s), pad = v2::Pad(size.x, s);
    const float x0 = win.x + side, x1 = win.x + size.x - side;
    const float hintHeight = (narrow ? 48 : 56) * s, hintTop = win.y + size.y - hintHeight;
    if (backdrop) DrawMenuBackdrop(true);
    bool backRequested = false;

    // Header: Home's brand, or the screen's title and its tabs.
    ImGui::SetCursorScreenPos(ImVec2(x0, win.y + pad));
    ImGui::PushItemWidth(x1 - x0);
    if (home) ImGui::Dummy(ImVec2(x1 - x0, Brand(d, ImGui::GetCursorScreenPos(), narrow) + 24 * s));
    else {
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(x1 - x0, 0));
        // Classic titles are capitals ("SETTINGS"); the V2 header names the screen as the menus do.
        const char* named = MenuScreenName(navigation.Screen());
        v2::Header(named && IsCapitals(title) ? named : title);
        if (!tabs.empty()) {
            std::vector<std::string> labels;
            for (const auto& t : tabs) labels.push_back(t.second);
            const int hit = v2::Tabs(labels, tab);
            if (hit >= 0 && hit != tab && !modalAtStart) action = {MenuAction::Shortcut, tabs[hit].first};
        }
        ImGui::EndGroup();
    }
    ImGui::PopItemWidth();
    const float top = ImGui::GetCursorScreenPos().y, bottom = hintTop - 16 * s;
    const ImVec2 region(x1 - x0, (std::max)(60.f, bottom - top));

    const auto focusedEntry = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == navigation.Focus(); });
    if (body) {
        ImGui::SetCursorScreenPos(ImVec2(x0, top));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("V2 body", region, 0, ImGuiWindowFlags_NoNavInputs);
        ImGui::BeginDisabled((modalAtStart && !choosingAtStart) || navigation.Editing() || navigation.Confirming());
        body(entries, navigation, action, region.y, feedback_);
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else if (navigation.Reading()) {
        ImGui::SetCursorScreenPos(ImVec2(x0, top));
        DrawReader(entries, detail, region.y, held);
    } else {
        // Home: the cards and a side column with the player. Elsewhere: the cards and the focused one's detail.
        const bool split = !narrow && region.x >= 820 * s;
        const float listWidth = home ? (split ? (std::min)(600 * s, region.x * .5f) : region.x) : split ? region.x * .55f : region.x;
        const bool twoLines = home || !split;
        const bool grid = columns > 1 && card;
        const std::size_t cells = grid ? MenuGridCells(entries) : 0;
        const auto mouse = ImGui::GetIO().MouseDelta;
        const bool pointerMoved = mouse.x != 0 || mouse.y != 0;
        ImVec2 focusMin, focusMax;
        ImGui::SetCursorScreenPos(ImVec2(x0, top));
        if (changed) ImGui::SetNextWindowScroll(ImVec2(0, navigation.Scroll()));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("V2 list", ImVec2(listWidth, region.y), 0, ImGuiWindowFlags_NoNavInputs);
        auto* ld = ImGui::GetWindowDrawList();
        const float gap = 8 * s, full = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        const float cellWidth = grid ? (full - gap * (columns - 1)) / columns : full;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            const bool focused = e.id == navigation.Focus(), enabled = feedback_.Enabled(e), cell = i < cells;
            if (cell && i % columns) ImGui::SameLine(0, gap);
            // A group's heading over its first card: caps, muted, with room above it after a group.
            if (!cell && !e.group.empty() && (i == 0 || entries[i - 1].group != e.group)) {
                if (i) ImGui::Dummy(ImVec2(0, 8 * s));
                const auto at = ImGui::GetCursorScreenPos();
                ld->AddText(HeadingFont(), 13 * s, ImVec2(at.x + 2 * s, at.y + 4 * s), palette::Muted, e.group.c_str());
                ImGui::Dummy(ImVec2(full, 18 * s));
            }
            const float w = cell ? cellWidth : full;
            const float h = cell ? cardHeight * s : e.height > 0 ? e.height * s : (twoLines && !e.detail.empty() ? 64 : 52) * s;
            const auto p = ImGui::GetCursorScreenPos();
            const ImVec2 q(p.x + w, p.y + h);
            ImGui::PushID(e.id.c_str());
            const bool pressed = ImGui::InvisibleButton("##card", ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            // The value's ends adjust it; anything else on the card is Select.
            const float valueWidth = e.adjustable ? (std::min)(220 * s, w * .4f) : 0;
            if (pressed && listEnabled && !modalAtStart && !navigation.Editing() && !navigation.Asking()) {
                navigation.Focus(e.id, entries);
                const float mx = ImGui::GetMousePos().x, vx = q.x - 16 * s - valueWidth;
                const int step = e.adjustable && e.enabled && mx >= vx ? (mx < vx + valueWidth * .5f ? -1 : 1) : 0;
                action = step ? MenuAction{MenuAction::Adjust, e.id, {}, step} : navigation.Choose(entries);
            } else if (listEnabled && pointerMoved && hovered) navigation.Focus(e.id, entries);
            if (focused) {
                focusMin = p; focusMax = q;
                if (lastFocus_ != e.id || changed) ImGui::SetScrollHereY(.5f);
            }
            if (!ImGui::IsItemVisible()) continue;
            ld->AddRectFilled(p, q, focused ? v2::color::Header : hovered ? v2::color::Hover : v2::color::Alpha(v2::color::Frame, .92f), 4 * s);
            ld->AddRect(p, q, v2::color::Alpha(v2::color::Border, .6f), 4 * s, 0, s);
            if (e.valueHighlighted) ld->AddRectFilled(p, ImVec2(p.x + 3 * s, q.y), palette::Ember, 4 * s, ImDrawFlags_RoundCornersLeft);
            if (cell && card(e, p, q)) {
                if (focused) v2::Focus(ld, p, q, false);
            } else {
                if (e.userText) NoteUserText(e.label);
                const ImU32 text = enabled ? palette::Ivory : v2::color::Alpha(palette::Ivory, .55f);
                const float face = e.Match() && !cell ? h - 12 * s : 0;
                if (face > 0) {
                    DrawCharacterPortrait(e.fighter1, ImVec2(p.x + 6 * s, p.y + 6 * s), ImVec2(p.x + 6 * s + face, q.y - 6 * s));
                    DrawCharacterPortrait(e.fighter2, ImVec2(q.x - 6 * s - face, p.y + 6 * s), ImVec2(q.x - 6 * s, q.y - 6 * s));
                }
                const float left = p.x + 16 * s + (face > 0 ? face + 6 * s : 0);
                const bool chevron = !e.adjustable && MenuSelectOpens(e) != SelectOpens::Nothing;
                const float right = q.x - 16 * s - (face > 0 ? face + 6 * s : 0) - (chevron ? 18 * s : 0);
                const std::string value = e.value;
                const float valueText = value.empty() || e.adjustable ? 0 : (std::min)(ImGui::CalcTextSize(value.c_str()).x, (right - left) * .45f);
                const float labelRoom = right - left - (e.adjustable ? valueWidth : valueText) - (valueText > 0 ? 16 * s : 0);
                ImFont* labelFont = home ? HeadingFont() : ImGui::GetFont();
                const float labelSize = (home ? 20 : 18) * s, detailSize = 15 * s;
                const bool second = twoLines && !e.detail.empty() && !cell;
                const float labelY = second ? p.y + 10 * s : p.y + (h - labelSize) * .5f;
                ld->AddText(labelFont, labelSize, ImVec2(left, labelY), text, Fit(e.label, labelRoom, labelSize, labelFont).c_str());
                if (second) {
                    NoteDetailText(e.detail, e.detailText);
                    ld->AddText(ImGui::GetFont(), detailSize, ImVec2(left, labelY + labelSize + 4 * s), palette::Muted,
                        Fit(FirstLine(e.detail), labelRoom, detailSize, ImGui::GetFont()).c_str());
                }
                const float valueY = p.y + (h - ImGui::GetFontSize()) * .5f;
                if (e.adjustable) {
                    const float vx = right - valueWidth;
                    const std::string shown = Fit(value.empty() ? loc::T("common.not_set") : value, valueWidth - 40 * s, ImGui::GetFontSize(), ImGui::GetFont());
                    const float tw = ImGui::CalcTextSize(shown.c_str()).x;
                    const ImU32 arrow = enabled ? palette::Ember : palette::Muted;
                    ld->AddText(ImVec2(vx, valueY), arrow, "<");
                    ld->AddText(ImVec2(vx + (valueWidth - tw) * .5f, valueY), e.valueHighlighted ? palette::Ember : text, shown.c_str());
                    ld->AddText(ImVec2(right - 10 * s, valueY), arrow, ">");
                } else if (valueText > 0) {
                    if (e.userText || e.text) NoteUserText(value);
                    const std::string shown = Fit(value, valueText, ImGui::GetFontSize(), ImGui::GetFont());
                    ld->AddText(ImVec2(right - ImGui::CalcTextSize(shown.c_str()).x, valueY), e.valueHighlighted ? palette::Ember : palette::Muted, shown.c_str());
                }
                if (chevron) Chevron(ld, ImVec2(q.x - 22 * s, p.y + h * .5f), 6 * s, enabled ? palette::Muted : v2::color::Alpha(palette::Muted, .5f));
                if (focused) v2::Focus(ld, p, q, !cell);
            }
        }
        ImGui::PopStyleVar();
        navigation.Scroll() = ImGui::GetScrollY();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        const auto listMin = ImGui::GetItemRectMin(), listMax = ImGui::GetItemRectMax();
        if (split) {
            const float paneX = home ? x1 - (std::min)(420 * s, region.x * .4f) : x0 + listWidth + 24 * s;
            const ImVec2 paneMin(paneX, top), paneSize(x1 - paneX, region.y);
            if (home) {
                // The player, as the V2 player card: the main's portrait and the tag.
                const auto& player = MenuPlayerCard();
                const float h = 128 * s, y = top + region.y - h;
                d->AddRectFilled(ImVec2(paneX, y), ImVec2(x1, y + h), v2::color::Alpha(v2::color::Panel, .94f), 4 * s);
                d->AddRect(ImVec2(paneX, y), ImVec2(x1, y + h), v2::color::Border, 4 * s, 0, s);
                const char* caps = loc::T("card.player");
                d->AddText(HeadingFont(), 13 * s, ImVec2(paneX + 16 * s, y + 14 * s), palette::Muted, caps);
                const float face = 72 * s, fy = y + 40 * s;
                DrawCharacterPortrait(player.fighter, ImVec2(paneX + 16 * s, fy), ImVec2(paneX + 16 * s + face, fy + face), v2::color::Frame);
                v2::TagView tag;
                tag.name = player.name.empty() ? loc::T("card.player") : player.name;
                tag.title = player.fighterName;
                tag.marks.country = player.country;
                tag.status = player.controllerReady ? loc::T("card.controller_ready") : loc::T("card.assign_controller");
                v2::Tag(d, ImVec2(paneX + 32 * s + face, fy + 4 * s), x1 - paneX - 48 * s - face, tag, v2::TagSize::M);
            } else if (focusedEntry != entries.end()) {
                ImGui::SetCursorScreenPos(paneMin);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(v2::color::Alpha(v2::color::Panel, .94f)));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 16 * s));
                ImGui::BeginChild("V2 detail", paneSize, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNavInputs);
                if (!customDetail) {
                    const auto& e = *focusedEntry;
                    ImGui::PushFont(HeadingFont());
                    if (e.userText) NoteUserText(e.label);
                    ImGui::TextWrapped("%s", e.label.c_str());
                    ImGui::PopFont();
                    ImGui::Dummy(ImVec2(0, 4 * s));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette::Muted));
                    if (!e.detail.empty()) { NoteDetailText(e.detail, e.detailText); ImGui::TextWrapped("%s", e.detail.c_str()); }
                    ImGui::PopStyleColor();
                    if (!feedback_.Enabled(e) && !e.quiet) ImGui::TextDisabled("%s", loc::T(feedback_.Pending(e) ? "room.updating" : "common.unavailable"));
                }
                if (detail) detail(focusedEntry->id);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
        }
        if (listCard.draw) DrawListCard(listMin, listMax, focusMin, focusMax, s);
    }
    if (action.kind == MenuAction::Back) { backRequested = true; action = {}; }

    // The hint bar: what the focused card's buttons do, each also a button; the status on the right.
    d->AddRectFilled(ImVec2(win.x, hintTop), ImVec2(win.x + size.x, win.y + size.y), v2::color::Alpha(v2::color::Ink, .92f));
    d->AddLine(ImVec2(win.x, hintTop), ImVec2(win.x + size.x, hintTop), v2::color::Border, s);
    MenuEntry shown;
    if (focusedEntry != entries.end()) { shown = *focusedEntry; shown.enabled = feedback_.Enabled(*focusedEntry); }
    const bool keys = KeyboardPrompts();
    struct Hint { const char* glyph; std::string label; int does; };  // does: 1 Select, 2 Back
    std::vector<Hint> hints;
    if (const char* primary = listEnabled ? MenuPrimaryHint(focusedEntry == entries.end() ? nullptr : &shown) : nullptr) hints.push_back({MenuSelectGlyph(), primary, 1});
    if (!backInert) hints.push_back({MenuBackGlyph(), backHint.empty() ? loc::T("common.back") : backHint, 2});
    if (shown.adjustable && shown.enabled) hints.push_back({keys ? "keys-horizontal" : "horizontal", loc::T("menu.adjust"), 0});
    for (const auto& h : shortcutHints) hints.push_back({h.glyph, h.label, 0});
    const float glyph = (narrow ? 26 : 30) * s, font = 16 * s, cy = hintTop + (hintHeight - glyph) * .5f;
    float hx = x0;
    for (const auto& h : hints) {
        const std::string label = PromptLabel(h.glyph, h.label, MenuPromptBrand());
        const float w = glyph + 8 * s + ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0, label.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(hx, hintTop));
        ImGui::PushID(h.glyph);
        if (ImGui::InvisibleButton("##hint", ImVec2(w, hintHeight)) && !modalAtStart && !noticeOpen) {
            if (h.does == 1 && focusedEntry != entries.end()) action = navigation.Choose(entries);
            if (h.does == 2) backRequested = true;
        }
        ImGui::PopID();
        DrawPromptGlyph(d, h.glyph, ImVec2(hx, cy), glyph, MenuArt(), s);
        d->AddText(ImGui::GetFont(), font, ImVec2(hx + glyph + 8 * s, hintTop + (hintHeight - font) * .5f), palette::Ivory, label.c_str());
        hx += w + 24 * s;
    }
    const std::string note = status && *status ? status : footerNote;
    if (!note.empty()) {
        const ImU32 tone = status && *status ? ImGui::ColorConvertFloat4ToU32(ToneColor(statusTone)) : palette::Muted;
        const std::string fitted = Fit(note, (std::max)(0.f, x1 - hx - 16 * s), 15 * s, ImGui::GetFont());
        const float tw = ImGui::GetFont()->CalcTextSizeA(15 * s, FLT_MAX, 0, fitted.c_str()).x;
        d->AddText(ImGui::GetFont(), 15 * s, ImVec2(x1 - tw, hintTop + (hintHeight - 15 * s) * .5f), tone, fitted.c_str());
    }
    return Finish(entries, action, backRequested, acceptEditText, static_cast<bool>(body), noticeOpen);
}
} }
