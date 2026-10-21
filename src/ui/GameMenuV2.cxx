// The V2 look of a menu screen (the V2 Overhaul design), cut from the window
// with v2::Box: the hint bar off the bottom, the brand or the title and its
// tabs off the top, then the cards with a detail pane (or, on Home, the
// player's card) beside them. A value's choices open in a panel on its card.
// Input, navigation and the dialogs are GameMenu's own, so a screen behaves
// the same in either look.
#include "GameMenu.hxx"
#include "MenuGlyphs.hxx"
#include "MenuPresentation.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <imgui_internal.h>
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
std::string FirstLine(const std::string& text) { return text.substr(0, text.find('\n')); }
void Chevron(ImDrawList* d, ImVec2 at, float size, ImU32 color) {
    d->AddPolyline(std::array<ImVec2, 3>{ImVec2(at.x, at.y - size), ImVec2(at.x + size * .6f, at.y), ImVec2(at.x, at.y + size)}.data(), 3, color, 0, 2 * Scale());
}
// Home's brand block: the emblem, EMBER and the subtitle.
void Brand(ImDrawList* d, ImVec2 p, float mark) {
    const float s = Scale(), size = mark * .78f;
    auto* atlas = ImGui::GetIO().Fonts;
    ImVec2 uv0, uv1;
    atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(0), &uv0, &uv1);
    d->AddImage(atlas->TexID, p, ImVec2(p.x + mark, p.y + mark), uv0, uv1);
    const float x = p.x + mark + 16 * s;
    d->AddText(HeadingFont(), size, ImVec2(x, p.y + (mark - size - 18 * s) * .5f), v2::Colors().text, "EMBER");
    d->AddText(HeadingFont(), 13 * s, ImVec2(x + 2 * s, p.y + (mark + size - 18 * s) * .5f + 4 * s), v2::Colors().accent, loc::T("brand.subtitle"));
}
// One entry's card: its label (and first line of detail on two-line cards), its
// value with the arrows that step it, or a chevron when Select opens something.
void DrawCardContent(ImDrawList* d, const MenuEntry& e, v2::Box c, bool enabled, bool twoLines, bool home, float valueWidth) {
    const float s = Scale(), h = c.H();
    if (e.userText) NoteUserText(e.label);
    const ImU32 text = enabled ? v2::Colors().text : v2::color::Alpha(v2::Colors().text, .55f);
    const float face = e.Match() ? h - 12 * s : 0;
    if (face > 0) {
        DrawCharacterPortrait(e.fighter1, ImVec2(c.min.x + 6 * s, c.min.y + 6 * s), ImVec2(c.min.x + 6 * s + face, c.max.y - 6 * s));
        DrawCharacterPortrait(e.fighter2, ImVec2(c.max.x - 6 * s - face, c.min.y + 6 * s), ImVec2(c.max.x - 6 * s, c.max.y - 6 * s));
    }
    const bool chevron = !e.adjustable && MenuSelectOpens(e) != SelectOpens::Nothing;
    const float left = c.min.x + 16 * s + (face > 0 ? face + 6 * s : 0);
    const float right = c.max.x - 16 * s - (face > 0 ? face + 6 * s : 0) - (chevron ? 18 * s : 0);
    const float valueText = e.value.empty() || e.adjustable ? 0 : (std::min)(ImGui::CalcTextSize(e.value.c_str()).x, (right - left) * .45f);
    const float labelRoom = right - left - (e.adjustable ? valueWidth : valueText) - (valueText > 0 ? 16 * s : 0);
    ImFont* labelFont = home ? HeadingFont() : ImGui::GetFont();
    const float labelSize = (home ? 20 : 18) * s, detailSize = 15 * s;
    const bool second = twoLines && !e.detail.empty();
    const float labelY = second ? c.min.y + 10 * s : c.min.y + (h - labelSize) * .5f;
    d->AddText(labelFont, labelSize, ImVec2(left, labelY), text, Fit(e.label, labelRoom, labelSize, labelFont).c_str());
    if (second) {
        NoteDetailText(e.detail, e.detailText);
        d->AddText(ImGui::GetFont(), detailSize, ImVec2(left, labelY + labelSize + 4 * s), v2::Colors().muted, Fit(FirstLine(e.detail), labelRoom, detailSize, ImGui::GetFont()).c_str());
    }
    const float valueY = c.min.y + (h - ImGui::GetFontSize()) * .5f;
    if (e.adjustable) {
        const float vx = right - valueWidth;
        const std::string shown = Fit(e.value.empty() ? loc::T("common.not_set") : e.value, valueWidth - 40 * s, ImGui::GetFontSize(), ImGui::GetFont());
        const float tw = ImGui::CalcTextSize(shown.c_str()).x;
        const ImU32 arrow = enabled ? v2::Colors().accent : v2::Colors().muted;
        d->AddText(ImVec2(vx, valueY), arrow, "<");
        d->AddText(ImVec2(vx + (valueWidth - tw) * .5f, valueY), e.valueHighlighted ? v2::Colors().accent : text, shown.c_str());
        d->AddText(ImVec2(right - 10 * s, valueY), arrow, ">");
    } else if (valueText > 0) {
        if (e.userText || e.text) NoteUserText(e.value);
        const std::string shown = Fit(e.value, valueText, ImGui::GetFontSize(), ImGui::GetFont());
        d->AddText(ImVec2(right - ImGui::CalcTextSize(shown.c_str()).x, valueY), e.valueHighlighted ? v2::Colors().accent : v2::Colors().muted, shown.c_str());
    }
    if (chevron) Chevron(d, ImVec2(c.max.x - 22 * s, c.min.y + h * .5f), 6 * s, enabled ? v2::Colors().muted : v2::color::Alpha(v2::Colors().muted, .5f));
}
}

MenuAction GameMenu::DrawV2(const char* title, const std::vector<MenuEntry>& entries, const char* status, const Detail& detail, int columns,
    const Card& card, const Body& body, float cardHeight, Tone statusTone, bool home, bool flyout, MenuAction action, unsigned held,
    bool modalAtStart, bool choosingAtStart, bool acceptEditText, bool noticeOpen) {
    const float s = Scale();
    auto* d = ImGui::GetWindowDrawList();
    const auto win = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    // What ImGui draws itself (a board's text and buttons) wears the brand's colours too.
    const auto& c = v2::Colors();
    const auto col = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    ImGui::PushStyleColor(ImGuiCol_Text, col(c.text));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, col(c.muted));
    ImGui::PushStyleColor(ImGuiCol_Button, col(c.button));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(c.hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(c.header));
    ImGui::PushStyleColor(ImGuiCol_Border, col(c.border));
    struct PopColors { ~PopColors() { ImGui::PopStyleColor(6); } } popColors;
    const bool narrow = v2::Narrow(size.x, s), changed = lastScreen_ != navigation.Screen();
    const float side = flyout ? 16 * s : v2::SideMargin(size.x, s), pad = flyout ? 12 * s : v2::Pad(size.x, s);
    // A flyout (the training controls) sits over the fight: no art behind it.
    if (backdrop && !flyout) DrawMenuBackdrop(true);
    bool backRequested = false;
    const auto focusedEntry = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == navigation.Focus(); });
    const auto mouse = ImGui::GetIO().MouseDelta;
    const bool pointerMoved = mouse.x != 0 || mouse.y != 0;

    v2::Box screen{win, ImVec2(win.x + size.x, win.y + size.y)};
    const v2::Box hintBar = v2::CutBottom(screen, (narrow || flyout ? 48 : 56) * s, (flyout ? 8 : 16) * s);
    v2::Box content{ImVec2(screen.min.x + side, screen.min.y + pad), ImVec2(screen.max.x - side, screen.max.y)};

    // Header: Home's brand, or the screen's title and its tabs.
    ImGui::SetCursorScreenPos(content.min);
    if (home) {
        const float mark = (narrow ? 44 : 72) * s;
        Brand(d, content.min, mark);
        v2::CutTop(content, mark, 24 * s);
    } else {
        ImGui::BeginGroup();
        // Classic titles are capitals ("SETTINGS"); the V2 header names the screen as the menus do.
        // A flyout's root is named by its owner (rootName), its pages as the menus name them.
        const char* named = navigation.Screen() == navigation.Root() && !rootName.empty() ? rootName.c_str() : MenuScreenName(navigation.Screen());
        v2::Header(named && IsCapitals(title) ? named : title, eyebrow.empty() ? nullptr : eyebrow.c_str(), headerChips);
        if (!tabs.empty()) {
            std::vector<std::string> labels;
            for (const auto& t : tabs) labels.push_back(t.second);
            const int hit = v2::Tabs(labels, tab);
            if (hit >= 0 && hit != tab && !modalAtStart) action = {MenuAction::Shortcut, tabs[hit].first};
        }
        ImGui::EndGroup();
        content.min.y = ImGui::GetCursorScreenPos().y;
    }

    bool valuePanel = false;
    if (body) {
        ImGui::SetCursorScreenPos(content.min);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("V2 body", ImVec2(content.W(), content.H()), 0, ImGuiWindowFlags_NoNavInputs);
        ImGui::BeginDisabled((modalAtStart && !choosingAtStart) || navigation.Editing() || navigation.Confirming());
        body(entries, navigation, action, content.H(), feedback_);
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else if (navigation.Reading()) {
        ImGui::SetCursorScreenPos(content.min);
        DrawReader(entries, detail, content.H(), held);
    } else {
        // Home: the cards and a column with the player. Elsewhere: the cards and the focused one's detail.
        const bool split = !narrow && content.W() >= 820 * s;
        v2::Box list = content, pane{};
        if (split) pane = v2::CutRight(list, home ? (std::min)(420 * s, content.W() * .4f) : content.W() * .45f - 24 * s, 24 * s);
        if (home && split) list.max.x = list.min.x + (std::min)(600 * s, list.W());
        // A flyout keeps to one line a card past its first page, so more of its settings show at once.
        const bool twoLines = home || (!split && (!flyout || navigation.Screen() == navigation.Root())), grid = columns > 1 && card;
        const std::size_t cells = grid ? MenuGridCells(entries) : 0;
        v2::Box focusCard{};
        ImGui::SetCursorScreenPos(list.min);
        if (changed) ImGui::SetNextWindowScroll(ImVec2(0, navigation.Scroll()));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("V2 list", ImVec2(list.W(), list.H()), 0, ImGuiWindowFlags_NoNavInputs);
        auto* ld = ImGui::GetWindowDrawList();
        const float gap = 8 * s, full = ImGui::GetContentRegionAvail().x;
        const float cellWidth = grid ? (full - gap * (columns - 1)) / columns : full;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            const bool focused = e.id == navigation.Focus(), enabled = feedback_.Enabled(e), cell = i < cells;
            if (cell && i % columns) ImGui::SameLine(0, gap);
            // A group's heading over its first card.
            if (!cell && !e.group.empty() && (i == 0 || entries[i - 1].group != e.group)) {
                if (i) ImGui::Dummy(ImVec2(0, 8 * s));
                const auto at = ImGui::GetCursorScreenPos();
                ld->AddText(HeadingFont(), 13 * s, ImVec2(at.x + 2 * s, at.y + 4 * s), v2::Colors().muted, e.group.c_str());
                ImGui::Dummy(ImVec2(full, 18 * s));
            }
            const float w = cell ? cellWidth : full;
            const float h = cell ? cardHeight * s : e.height > 0 ? e.height * s : (twoLines && !e.detail.empty() ? 64 : 52) * s;
            const auto p = ImGui::GetCursorScreenPos();
            const v2::Box c{p, ImVec2(p.x + w, p.y + h)};
            ImGui::PushID(e.id.c_str());
            const bool pressed = ImGui::InvisibleButton("##card", ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            // The value's ends step it; anything else on the card is Select.
            const float valueWidth = e.adjustable ? (std::min)(220 * s, w * .4f) : 0;
            if (pressed && listEnabled && !modalAtStart && !navigation.Editing() && !navigation.Asking()) {
                navigation.Focus(e.id, entries);
                const float mx = ImGui::GetMousePos().x, vx = c.max.x - 16 * s - valueWidth;
                const int step = e.adjustable && e.enabled && mx >= vx ? (mx < vx + valueWidth * .5f ? -1 : 1) : 0;
                action = step ? MenuAction{MenuAction::Adjust, e.id, {}, step} : navigation.Choose(entries);
            } else if (listEnabled && pointerMoved && hovered && !navigation.Choosing()) navigation.Focus(e.id, entries);
            if (focused) {
                focusCard = c;
                if (lastFocus_ != e.id || changed) ImGui::SetScrollHereY(.5f);
            }
            if (!ImGui::IsItemVisible()) continue;
            v2::CardGround(ld, c, focused, hovered, e.valueHighlighted);
            // A screen's own drawing of its rows (portraits, flags, rankings), as Classic draws them.
            if (card && card(e, c.min, c.max)) { if (focused) v2::Focus(ld, c.min, c.max, !cell); continue; }
            DrawCardContent(ld, e, c, enabled, twoLines && !cell, home, valueWidth);
            if (focused) v2::Focus(ld, c.min, c.max, !cell);
        }
        ImGui::PopStyleVar();
        navigation.Scroll() = ImGui::GetScrollY();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        if (split && home) {
            // The player, as the V2 player card: the main's portrait and the tag.
            const auto& player = MenuPlayerCard();
            const v2::Box pc = v2::CutBottom(pane, 128 * s);
            d->AddRectFilled(pc.min, pc.max, v2::color::Alpha(v2::Colors().panel, .94f), 4 * s);
            d->AddRect(pc.min, pc.max, v2::Colors().border, 4 * s, 0, s);
            d->AddText(HeadingFont(), 13 * s, ImVec2(pc.min.x + 16 * s, pc.min.y + 14 * s), v2::Colors().muted, loc::T("card.player"));
            const float face = 72 * s, fy = pc.min.y + 40 * s;
            DrawCharacterPortrait(player.fighter, ImVec2(pc.min.x + 16 * s, fy), ImVec2(pc.min.x + 16 * s + face, fy + face), v2::Colors().frame);
            v2::TagView tag;
            tag.name = player.name.empty() ? loc::T("card.player") : player.name;
            tag.title = player.fighterName;
            tag.marks.country = player.country;
            tag.status = player.controllerReady ? loc::T("card.controller_ready") : loc::T("card.assign_controller");
            v2::Tag(d, ImVec2(pc.min.x + 32 * s + face, fy + 4 * s), pc.W() - 48 * s - face, tag, v2::TagSize::M);
        } else if (split && focusedEntry != entries.end()) {
            ImGui::SetCursorScreenPos(pane.min);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(v2::color::Alpha(v2::Colors().panel, .94f)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 16 * s));
            ImGui::BeginChild("V2 detail", ImVec2(pane.W(), pane.H()), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNavInputs);
            if (!customDetail) {
                const auto& e = *focusedEntry;
                ImGui::PushFont(HeadingFont());
                if (e.userText) NoteUserText(e.label);
                ImGui::TextWrapped("%s", e.label.c_str());
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 4 * s));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(v2::Colors().muted));
                if (!e.detail.empty()) { NoteDetailText(e.detail, e.detailText); ImGui::TextWrapped("%s", e.detail.c_str()); }
                ImGui::PopStyleColor();
                if (!feedback_.Enabled(e) && !e.quiet) ImGui::TextDisabled("%s", loc::T(feedback_.Pending(e) ? "room.updating" : "common.unavailable"));
            }
            if (detail) detail(focusedEntry->id);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        // A value's choices: a panel on its own card, inside the cards' column, so
        // the detail pane and its preview stay in view. Picking one closes it.
        if (navigation.Choosing() && focusedEntry != entries.end() && focusCard.W() > 0 && notice_.empty()) {
            valuePanel = true;
            const auto& e = *focusedEntry;
            const std::size_t count = e.choices.size(), across = MenuChoiceColumns(count), lines = (count + across - 1) / across;
            const float row = 44 * s, inner = 8 * s, height = lines * row + (lines - 1) * inner + 24 * s + 22 * s;
            // Under the card when it fits, else over it; always inside the column.
            float top = focusCard.max.y + 4 * s;
            if (top + height > list.max.y) top = (std::max)(list.min.y, focusCard.min.y - 4 * s - height);
            ImGui::SetNextWindowPos(ImVec2(list.min.x, top));
            ImGui::SetNextWindowSize(ImVec2(list.W(), (std::min)(height, list.H())));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * s, 12 * s));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4 * s);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(v2::Colors().panel));
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(v2::Colors().accent));
            ImGui::Begin("###V2 values", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            auto* pd = ImGui::GetWindowDrawList();
            const auto at = ImGui::GetCursorScreenPos();
            pd->AddText(HeadingFont(), 13 * s, ImVec2(at.x + 2 * s, at.y), v2::Colors().muted, e.label.c_str());
            ImGui::Dummy(ImVec2(0, 14 * s));
            const float cw = (ImGui::GetContentRegionAvail().x - inner * (across - 1)) / across;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(inner, inner));
            for (std::size_t i = 0; i < count && navigation.Choosing(); ++i) {
                const auto& option = e.choices[i];
                const bool selected = i == navigation.ChoiceIndex(), current = option.id == e.chosen;
                if (i % across) ImGui::SameLine();
                const auto p = ImGui::GetCursorScreenPos();
                const v2::Box c{p, ImVec2(p.x + cw, p.y + row)};
                ImGui::PushID(static_cast<int>(i));
                const bool pressed = ImGui::InvisibleButton("##value", ImVec2(cw, row));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (pressed && option.enabled) action = navigation.Pick(i, entries);
                else if (hovered && pointerMoved) navigation.ChoiceIndex(i);
                v2::CardGround(pd, c, selected, hovered, false);
                const std::string label = Fit(option.label, cw - 24 * s, ImGui::GetFontSize(), ImGui::GetFont());
                // The value in effect now is in Ember, so the player sees where they start.
                pd->AddText(ImVec2(p.x + (cw - ImGui::CalcTextSize(label.c_str()).x) * .5f, p.y + (row - ImGui::GetFontSize()) * .5f),
                    !option.enabled ? v2::Colors().muted : current ? v2::Colors().accent : v2::Colors().text, label.c_str());
                if (selected) v2::Focus(pd, c.min, c.max, false);
            }
            ImGui::PopStyleVar();
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }
        if (listCard.draw) DrawListCard(list.min, list.max, focusCard.min, focusCard.max, s);
    }
    if (action.kind == MenuAction::Back) { backRequested = true; action = {}; }

    // The hint bar: what the focused card's buttons do, each also a button; the status on the right.
    MenuEntry shown;
    if (focusedEntry != entries.end()) { shown = *focusedEntry; shown.enabled = feedback_.Enabled(*focusedEntry); }
    std::vector<v2::Hint> hints;
    std::vector<int> does;  // 1 Select, 2 Back, 3 previous tab, 4 next tab
    const bool choosing = navigation.Choosing();
    if (const char* primary = !listEnabled ? nullptr : choosing ? loc::T("menu.choose") : MenuPrimaryHint(focusedEntry == entries.end() ? nullptr : &shown)) {
        hints.push_back({MenuSelectGlyph(), primary}); does.push_back(1);
    }
    if (!backInert) { hints.push_back({MenuBackGlyph(), choosing ? loc::T("common.cancel") : backHint.empty() ? loc::T("common.back") : backHint}); does.push_back(2); }
    if (shown.adjustable && shown.enabled && !choosing) { hints.push_back({KeyboardPrompts() ? "keys-horizontal" : "horizontal", loc::T("menu.adjust")}); does.push_back(0); }
    for (const auto& h : shortcutHints) { hints.push_back({h.glyph, h.label}); does.push_back(0); }
    // Tabs step with LB and RB (Page Up and Down on the keyboard).
    if (!tabs.empty() && !choosing) {
        const bool keys = KeyboardPrompts();
        hints.push_back({keys ? "PgUp" : "LB", ""}); does.push_back(3);
        hints.push_back({keys ? "PgDn" : "RB", loc::T("menu.pages")}); does.push_back(4);
    }
    const bool hasStatus = status && *status;
    const int hit = v2::HintBar(hintBar, hints, hasStatus ? status : footerNote,
        hasStatus ? ImGui::ColorConvertFloat4ToU32(ToneColor(statusTone)) : v2::Colors().muted);
    if (hit >= 0 && !modalAtStart && !noticeOpen) {
        if (does[hit] == 2) { if (choosing) navigation.Cancel(); else backRequested = true; }
        else if (does[hit] == 1 && focusedEntry != entries.end() && !choosing) action = navigation.Choose(entries);
        else if ((does[hit] == 3 || does[hit] == 4) && !modalAtStart) {
            const int next = tab + (does[hit] == 4 ? 1 : -1);
            if (next >= 0 && next < static_cast<int>(tabs.size())) action = {MenuAction::Shortcut, tabs[next].first};
        }
    }
    return Finish(entries, action, backRequested, acceptEditText, static_cast<bool>(body) || valuePanel, noticeOpen);
}
} }
