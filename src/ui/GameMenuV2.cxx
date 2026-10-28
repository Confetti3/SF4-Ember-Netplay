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
using v2::Fit;
// A title written in capitals, as the classic headers are.
bool IsCapitals(const char* text) {
    bool letter = false;
    for (; *text; ++text) { if (*text >= 'a' && *text <= 'z') return false; letter |= *text >= 'A' && *text <= 'Z'; }
    return letter;
}
std::string FirstLine(const std::string& text) { return text.substr(0, text.find('\n')); }
using v2::Chevron;
// Home's brand block: the emblem, EMBER and the subtitle.
void Brand(ImDrawList* d, ImVec2 p, float mark) {
    const float s = Scale(), size = mark * .78f;
    const float x = p.x + v2::BrandMark(d, p, mark) + 16 * s;
    d->AddText(HeadingFont(), size, ImVec2(x, p.y + (mark - size - 18 * s) * .5f), v2::Colors().text, "EMBER");
    d->AddText(HeadingFont(), 13 * s, ImVec2(x + 2 * s, p.y + (mark + size - 18 * s) * .5f + 4 * s), v2::Colors().accent, loc::T("brand.subtitle"));
}
// One entry's card: its label (and first line of detail on two-line cards), its
// value with the arrows that step it, or a chevron when Select opens something.
// With `chip` a value that is not a setting stands as an Ember chip (Online play's "B+").
void DrawCardContent(ImDrawList* d, const MenuEntry& e, v2::Box c, bool enabled, bool twoLines, bool home, float valueWidth, bool chip) {
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
    chip = chip && !e.info && !e.value.empty() && !e.adjustable;
    const float valueText = e.value.empty() || e.adjustable ? 0 : (std::min)(chip ? v2::ChipWidth(e.value) : ImGui::CalcTextSize(e.value.c_str()).x, (right - left) * .45f);
    const float labelRoom = right - left - (e.adjustable ? valueWidth : valueText) - (valueText > 0 ? 16 * s : 0);
    ImFont* labelFont = home ? HeadingFont() : ImGui::GetFont();
    const float labelSize = (home ? 20 : 18) * s, detailSize = 15 * s;
    const bool second = twoLines && !e.detail.empty();
    const float labelY = second ? c.min.y + 10 * s : c.min.y + (h - labelSize) * .5f;
    d->AddText(labelFont, labelSize, ImVec2(left, labelY), text, Fit(e.label, labelRoom, labelSize, labelFont).c_str());
    if (second) {
        NoteDetailText(e.detail, e.detailText);
        // A card's detail may shorten: the detail pane and speech give it whole.
        d->AddText(ImGui::GetFont(), detailSize, ImVec2(left, labelY + labelSize + 4 * s), v2::Colors().muted, Fit(FirstLine(e.detail), labelRoom, detailSize, ImGui::GetFont(), false).c_str());
    }
    const float valueY = c.min.y + (h - ImGui::GetFontSize()) * .5f;
    if (e.adjustable) {
        DrawMenuValue(d, e, {ImVec2(right - valueWidth, c.min.y), ImVec2(right, c.max.y)}, enabled, text);
    } else if (chip) {
        v2::Chip(d, ImVec2(right - valueText, c.min.y + (h - ImGui::GetFontSize() * .78f - 8 * s) * .5f), e.value, v2::Colors().accent);
    } else if (valueText > 0) {
        if (e.userText || e.text) NoteUserText(e.value);
        const std::string shown = Fit(e.value, valueText, ImGui::GetFontSize(), ImGui::GetFont());
        d->AddText(ImVec2(right - ImGui::CalcTextSize(shown.c_str()).x, valueY), e.valueHighlighted ? v2::Colors().accent : v2::Colors().muted, shown.c_str());
    }
    if (chevron) Chevron(d, ImVec2(c.max.x - 22 * s, c.min.y + h * .5f), 6 * s, enabled ? v2::Colors().muted : v2::color::Alpha(v2::Colors().muted, .5f));
}
}

bool MenuValueIsToggle(const MenuEntry& e) { return e.adjustable && (e.value == loc::T("common.on") || e.value == loc::T("common.off")); }
void DrawMenuValue(ImDrawList* d, const MenuEntry& e, v2::Box box, bool enabled, ImU32 text) {
    const float s = Scale(), h = box.H(), y = box.min.y + (h - ImGui::GetFontSize()) * .5f;
    const auto& col = v2::Colors();
    if (MenuValueIsToggle(e)) {
        const bool on = e.value == loc::T("common.on");
        const float tw = 36 * s, th = 20 * s, ty = box.min.y + (h - th) * .5f, tx = box.max.x - tw;
        d->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), on ? col.accent : col.button, th * .5f);
        d->AddRect(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), on ? col.accent : col.border, th * .5f, 0, s);
        d->AddCircleFilled(ImVec2(on ? tx + tw - th * .5f : tx + th * .5f, ty + th * .5f), 7 * s, enabled ? col.text : col.muted);
        return;
    }
    const std::string value = e.value.empty() ? loc::T("common.not_set") : e.value;
    const ImU32 shown = e.valueHighlighted ? col.accent : text;
    if (!MenuValueIsNumber(e)) {
        const std::string fit = Fit(value, box.W() - 20 * s, ImGui::GetFontSize(), ImGui::GetFont());
        d->AddText(ImVec2(box.max.x - 18 * s - ImGui::CalcTextSize(fit.c_str()).x, y), shown, fit.c_str());
        Chevron(d, ImVec2(box.max.x - 6 * s, box.min.y + h * .5f), 6 * s, enabled ? col.muted : v2::color::Alpha(col.muted, .5f));
        return;
    }
    const std::string fit = Fit(value, box.W() - 40 * s, ImGui::GetFontSize(), ImGui::GetFont());
    const ImU32 arrow = enabled ? col.accent : col.muted;
    d->AddText(ImVec2(box.min.x, y), arrow, "<");
    d->AddText(ImVec2(box.min.x + (box.W() - ImGui::CalcTextSize(fit.c_str()).x) * .5f, y), shown, fit.c_str());
    d->AddText(ImVec2(box.max.x - 10 * s, y), arrow, ">");
}

void DrawMenuRow(ImDrawList* d, const MenuEntry& e, v2::Box r, bool enabled, float valueWidth, float inset) {
    const float s = Scale(), font = ImGui::GetFontSize(), y = r.min.y + (r.H() - font) * .5f, right = r.max.x - inset;
    const auto& c = v2::Colors();
    const ImU32 text = enabled ? c.text : c.muted;
    // A switch takes only its own width, so its label has the rest.
    const float value = MenuValueIsToggle(e) ? 36 * s : e.adjustable || !e.value.empty() ? valueWidth : -inset - 8 * s;
    v2::Text(d, ImVec2(r.min.x + inset, y), right - value - 8 * s - r.min.x - inset, e.label, font, text);
    if (e.userText || e.text) NoteUserText(e.value);
    if (e.adjustable) { DrawMenuValue(d, e, {ImVec2(right - valueWidth, r.min.y), ImVec2(right, r.max.y)}, enabled, text); return; }
    // A fixed value, then the chevron of a row Select opens (an info row has none), as DrawMenuValue draws them.
    const float end = e.info ? right : right - 18 * s;
    if (!e.value.empty()) {
        const std::string shown = Fit(e.value, valueWidth - 20 * s, font, ImGui::GetFont());
        d->AddText(ImVec2(end - ImGui::CalcTextSize(shown.c_str()).x, y), e.valueHighlighted ? c.accent : e.info ? c.muted : text, shown.c_str());
    }
    if (!e.info) Chevron(d, ImVec2(right - 6 * s, r.min.y + r.H() * .5f), 6 * s, enabled ? c.muted : v2::color::Alpha(c.muted, .5f));
}
EntryHit PressEntry(MenuNavigation& nav, const std::vector<MenuEntry>& rows, const MenuEntry& e, v2::Box b, MenuAction& action,
    bool scroll, float valueWidth, bool choose) {
    ImGui::SetCursorScreenPos(b.min);
    ImGui::PushID(e.id.c_str());
    const bool pressed = ImGui::InvisibleButton("##entry", ImVec2(b.W(), b.H())) && !nav.Asking() && !nav.Editing();
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (scroll && e.id == nav.Focus()) ImGui::SetScrollHereY(.5f);
    const auto mouse = ImGui::GetIO().MouseDelta;
    if (pressed && choose) {
        nav.Focus(e.id, rows);
        // The value's ends step it; anything else on the row is Select.
        const float mx = ImGui::GetMousePos().x, vx = b.max.x - 14 * Scale() - valueWidth;
        const int step = valueWidth > 0 && MenuValueIsNumber(e) && e.enabled && mx >= vx ? (mx < vx + valueWidth * .5f ? -1 : 1) : 0;
        action = step ? MenuAction{MenuAction::Adjust, e.id, {}, step} : nav.Choose(rows);
    } else if (hovered && (mouse.x != 0 || mouse.y != 0) && !nav.Choosing()) nav.Focus(e.id, rows);
    return {pressed, hovered};
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
        // Its Back button is the hint bar's Back, so it closes an open question first.
        if (v2::Header(named && IsCapitals(title) ? named : title, eyebrow.empty() ? nullptr : eyebrow.c_str(), headerChips, backInert ? nullptr : loc::T("common.back"))
            && !modalAtStart && !noticeOpen && !navigation.Editing()) {
            if (navigation.Asking()) navigation.Cancel();
            else backRequested = true;
        }
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
    // A value's choices: a panel on its own card, inside the cards' column, so
    // the detail pane and its preview stay in view. Picking one closes it.
    const auto Values = [&](v2::Box focusCard, v2::Box list) {
        if (popoutClose_ && navigation.Choosing()) navigation.Cancel();
        popoutClose_ = false;
        if (!navigation.Choosing() || focusedEntry == entries.end() || focusCard.W() <= 0 || !notice_.empty()) { popoutOpen_ = false; return false; }
        const auto& e = *focusedEntry;
        // Select on a switch flips it: no popup for two words.
        if (MenuValueIsToggle(e)) { navigation.Cancel(); action = {MenuAction::Adjust, e.id, {}, e.value == loc::T("common.on") ? -1 : 1}; return false; }
        const auto grid = MenuChoiceLayout(e.choices);
        bool closeOutside = false;
        const std::size_t count = e.choices.size(), across = e.popout ? 1 : grid.across, lines = (count + across - 1) / across;
        const float row = (e.popout ? 44 : 48) * s, inner = (e.popout ? 8 : 6) * s, foot = e.popout ? 0 : 30 * s;
        const float height = lines * row + (lines - 1) * inner + 24 * s + (e.popout ? 22 : 38) * s + foot;
        if (e.popout) {
            // A popout opens where it was asked for: at the click, or beside the row for the pad
            // and keys, and turns back at the screen's edges.
            if (!popoutOpen_) popoutAt_ = ImGui::IsMouseReleased(ImGuiMouseButton_Left) ? ImGui::GetMousePos() : ImVec2(focusCard.max.x - 24 * s, focusCard.min.y);
            else closeOutside = true;
            popoutOpen_ = true;
            const float w = 300 * s, h = (std::min)(height, size.y - 16 * s);
            ImVec2 at = popoutAt_;
            if (at.x + w > win.x + size.x - 8 * s) at.x = (std::max)(win.x + 8 * s, at.x - w);
            if (at.y + h > win.y + size.y - 8 * s) at.y = (std::max)(win.y + 8 * s, win.y + size.y - 8 * s - h);
            ImGui::SetNextWindowPos(at);
            ImGui::SetNextWindowSize(ImVec2(w, h));
        } else {
            // At the column's right edge, level with the row, kept inside the column.
            const auto chosen = std::find_if(e.choices.begin(), e.choices.end(), [&](const MenuChoice& o) { return o.id == e.chosen; });
            const float keys = ImGui::CalcTextSize(loc::Tf("menu.choices_keys", chosen == e.choices.end() ? e.value : chosen->label).c_str()).x + 24 * s;
            const float w = (std::min)(list.W(), (std::max)(keys, across * grid.cell + (across - 1) * inner + 24 * s));
            const float top = (std::max)(list.min.y, (std::min)(focusCard.min.y, list.max.y - height));
            ImGui::SetNextWindowPos(ImVec2(list.max.x - w - 12 * s, top));
            ImGui::SetNextWindowSize(ImVec2(w, (std::min)(height, list.H())));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * s, 12 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4 * s);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(v2::Colors().panel));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(v2::Colors().accent));
        ImGui::Begin("###V2 values", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        auto* pd = ImGui::GetWindowDrawList();
        const auto at = ImGui::GetCursorScreenPos();
        const auto& col = v2::Colors();
        if (e.popout) {
            v2::SectionHeading(pd, ImVec2(at.x + 2 * s, at.y), Fit(e.label, ImGui::GetContentRegionAvail().x - 2 * s, 14 * s, HeadingFont()));
            ImGui::Dummy(ImVec2(0, 14 * s));
        } else {
            // The title and its count over a rule, the accent's bar down the left.
            const auto wp = ImGui::GetWindowPos();
            pd->AddRectFilled(wp, ImVec2(wp.x + 3 * s, wp.y + ImGui::GetWindowHeight()), col.accent);
            const float avail = ImGui::GetContentRegionAvail().x;
            const std::string counted = loc::Tf("menu.choices_count", count);
            pd->AddText(HeadingFont(), 18 * s, ImVec2(at.x + 4 * s, at.y), col.text, Fit(e.label, avail - ImGui::CalcTextSize(counted.c_str()).x - 16 * s, 18 * s, HeadingFont()).c_str());
            pd->AddText(ImVec2(at.x + avail - ImGui::CalcTextSize(counted.c_str()).x - 4 * s, at.y + 2 * s), col.muted, counted.c_str());
            pd->AddLine(ImVec2(at.x, at.y + 28 * s), ImVec2(at.x + avail, at.y + 28 * s), col.border, s);
            ImGui::Dummy(ImVec2(0, 30 * s));
        }
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
            // The value in effect now is ticked, so the player sees where they start.
            const float tick = current ? 22 * s : 0;
            if (current) pd->AddPolyline(std::array<ImVec2, 3>{ImVec2(p.x + 12 * s, p.y + row * .5f), ImVec2(p.x + 16 * s, p.y + row * .5f + 4 * s), ImVec2(p.x + 23 * s, p.y + row * .5f - 5 * s)}.data(), 3, col.accent, 0, 2.5f * s);
            const std::string label = Fit(option.label, cw - 24 * s - tick, ImGui::GetFontSize(), ImGui::GetFont());
            pd->AddText(ImVec2(p.x + 12 * s + tick, p.y + (row - ImGui::GetFontSize()) * .5f), !option.enabled ? col.muted : col.text, label.c_str());
            if (selected) v2::Focus(pd, c.min, c.max, false);
            if (selected && lastChoice_ != i) ImGui::SetScrollHereY(.5f);
        }
        lastChoice_ = navigation.ChoiceIndex();
        // A click outside an open popout closes it. The board under it was drawn
        // while it was still open, so that click does nothing else.
        // It closes on the next frame, after the hint bar's Back has had this click.
        popoutClose_ = closeOutside && navigation.Choosing() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered();
        ImGui::PopStyleVar();
        if (!e.popout && navigation.Choosing()) {
            const auto chosen = std::find_if(e.choices.begin(), e.choices.end(), [&](const MenuChoice& o) { return o.id == e.chosen; });
            ImGui::Dummy(ImVec2(0, 2 * s));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col.muted), "%s", loc::Tf("menu.choices_keys", chosen == e.choices.end() ? e.value : chosen->label).c_str());
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        return true;
    };
    if (body && !navigation.Reading()) {
        ImGui::SetCursorScreenPos(content.min);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("V2 body", ImVec2(content.W(), content.H()), 0, ImGuiWindowFlags_NoNavInputs);
        ImGui::BeginDisabled((modalAtStart && !choosingAtStart) || navigation.Editing() || navigation.Confirming());
        bodyChoices = {};
        body(entries, navigation, action, content.H(), feedback_);
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        valuePanel = Values(bodyChoices.card, bodyChoices.column);
    } else if (navigation.Reading()) {
        ImGui::SetCursorScreenPos(content.min);
        DrawReader(entries, detail, content.H(), held);
    } else {
        // Home: the cards and a column with the player. Elsewhere: the cards and the focused one's detail.
        const bool split = !narrow && content.W() >= 820 * s;
        v2::Box list = content, pane{};
        if (split) {
            const float w = home ? (std::min)(460 * s, content.W() * .42f) : listWidth > 0 ? content.W() - (std::min)(listWidth * s, content.W() * .55f) - 24 * s :
                paneWidth > 0 ? (std::min)(paneWidth * s, content.W() * .45f) : content.W() * .45f - 24 * s;
            pane = paneLeft ? v2::CutLeft(list, w, 24 * s) : v2::CutRight(list, w, 24 * s);
        }
        if (home && split) list.max.x = list.min.x + (std::min)(600 * s, list.W());
        // A flyout keeps to one line a card past its first page, so more of its settings show at once.
        const bool twoLines = home || listWidth > 0 || (!split && (!flyout || navigation.Screen() == navigation.Root())), grid = columns > 1 && card;
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
            // Home's "awards" row is the player's card in the pane.
            if (home && split && e.id == "awards") continue;
            const bool focused = e.id == navigation.Focus(), enabled = feedback_.Enabled(e), cell = i < cells;
            if (cell && i % columns) ImGui::SameLine(0, gap);
            // A group's heading over its first card.
            if (!cell && !e.group.empty() && (i == 0 || entries[i - 1].group != e.group)) {
                if (i) ImGui::Dummy(ImVec2(0, 8 * s));
                const auto at = ImGui::GetCursorScreenPos();
                v2::SectionHeading(ld, ImVec2(at.x + 2 * s, at.y + 4 * s), e.group);
                ImGui::Dummy(ImVec2(full, 18 * s));
            }
            const float w = cell ? cellWidth : full;
            const float h = cell ? cardHeight * s : e.height > 0 ? e.height * s : (twoLines && !e.detail.empty() ? listWidth > 0 ? 68 : 64 : 52) * s;
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
                const int step = MenuValueIsNumber(e) && e.enabled && mx >= vx ? (mx < vx + valueWidth * .5f ? -1 : 1) : 0;
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
            DrawCardContent(ld, e, c, enabled, twoLines && !cell, home, valueWidth, listWidth > 0);
            if (focused) v2::Focus(ld, c.min, c.max, !cell);
        }
        ImGui::PopStyleVar();
        navigation.Scroll() = ImGui::GetScrollY();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        if (split && home) {
            // The player's card (the L tag: title, icon, facts) on top, the news under it.
            const auto& c = v2::Colors();
            const float cardHeight = v2::Tag(d, pane.min, pane.W(), MenuPlayerCard().profile, v2::TagSize::L);
            NoteUserText(MenuPlayerCard().profile.title);
            // The card is the "awards" row: Select, or a click, opens the awards wall.
            const auto row = std::find_if(entries.begin(), entries.end(), [](const MenuEntry& e) { return e.id == "awards"; });
            if (row != entries.end()) {
                const v2::Box card{pane.min, ImVec2(pane.max.x, pane.min.y + cardHeight)};
                ImGui::SetCursorScreenPos(card.min);
                const bool pressed = ImGui::InvisibleButton("##player-card", ImVec2(card.W(), card.H())), hovered = ImGui::IsItemHovered();
                if (pressed && listEnabled && !modalAtStart && !navigation.Editing() && !navigation.Asking()) { navigation.Focus(row->id, entries); action = navigation.Choose(entries); }
                else if (listEnabled && pointerMoved && hovered && !navigation.Choosing()) navigation.Focus(row->id, entries);
                if (row->id == navigation.Focus()) v2::Focus(d, card.min, card.max);
            }
            v2::CutTop(pane, cardHeight, 16 * s);
            const auto& news = MenuNews();
            const float head = 40 * s, item = 56 * s;
            const std::size_t shown = (std::min)(news.size(), static_cast<std::size_t>((std::max)(0.f, (pane.H() - head - 36 * s) / item)));
            if (shown) {
                const v2::Box box = v2::CutBottom(pane, head + shown * item + 36 * s);
                d->AddRectFilled(box.min, box.max, v2::PanelGround(), 4 * s);
                d->AddText(HeadingFont(), 13 * s, ImVec2(box.min.x + 16 * s, box.min.y + 14 * s), c.muted, loc::T("news.heading"));
                d->AddLine(ImVec2(box.min.x, box.min.y + head), ImVec2(box.max.x, box.min.y + head), c.border, s);
                for (std::size_t i = 0; i < shown; ++i) {
                    const float y = box.min.y + head + i * item;
                    NoteUserText(news[i].title);
                    d->AddText(HeadingFont(), 16 * s, ImVec2(box.min.x + 16 * s, y + 10 * s), c.text, Fit(news[i].title, box.W() - 32 * s, 16 * s, HeadingFont()).c_str());
                    d->AddText(ImGui::GetFont(), 13 * s, ImVec2(box.min.x + 16 * s, y + 32 * s), c.muted, news[i].meta.c_str());
                    d->AddLine(ImVec2(box.min.x, y + item), ImVec2(box.max.x, y + item), v2::color::Alpha(c.border, .6f), s);
                }
                d->AddText(ImGui::GetFont(), 14 * s, ImVec2(box.min.x + 16 * s, box.max.y - 26 * s), c.muted, loc::T("news.more"));
                d->AddRect(box.min, box.max, c.border, 4 * s, 0, s);
            }
        } else if (split && focusedEntry != entries.end()) {
            ImGui::SetCursorScreenPos(pane.min);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, paneLeft ? ImVec4(0, 0, 0, 0) : ImGui::ColorConvertU32ToFloat4(v2::PanelGround()));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 16 * s));
            // Beside Online play's cards the pane is as tall as what it says; a screen's own pane (News) takes the height.
            const bool fit = listWidth > 0 && !customDetail;
            if (fit) ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, pane.H()));
            ImGui::BeginChild("V2 detail", ImVec2(pane.W(), fit ? 0 : pane.H()),
                ImGuiChildFlags_AlwaysUseWindowPadding | (fit ? ImGuiChildFlags_AutoResizeY : 0), ImGuiWindowFlags_NoNavInputs);
            if (!customDetail) {
                const auto& e = *focusedEntry;
                if (e.userText) NoteUserText(e.label);
                v2::Paragraph(e.label, 22 * s, v2::Colors().text, HeadingFont());
                if (!e.detail.empty()) { NoteDetailText(e.detail, e.detailText); v2::Paragraph(e.detail, 16 * s, v2::Colors().muted); }
                if (!feedback_.Enabled(e) && !e.quiet) ImGui::TextDisabled("%s", loc::T(feedback_.Pending(e) ? "room.updating" : "common.unavailable"));
            }
            if (detail) detail(focusedEntry->id);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        valuePanel = Values(focusCard, list);
        if (listCard.draw) DrawListCard(list.min, list.max, focusCard.min, focusCard.max, s);
    }
    if (action.kind == MenuAction::Back) { backRequested = true; action = {}; }
    // The right mouse button is Back, as B and Escape: it closes an open value panel or question first.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !noticeOpen && !navigation.Editing()) {
        if (navigation.Asking()) navigation.Cancel();
        else if (!backInert) backRequested = true;
    }

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
    if (MenuValueIsNumber(shown) && shown.enabled && !choosing) { hints.push_back({KeyboardPrompts() ? "keys-horizontal" : "horizontal", loc::T("menu.adjust")}); does.push_back(0); }
    for (const auto& h : shortcutHints) { hints.push_back({h.glyph, h.label}); does.push_back(0); }
    // Tabs step with LB and RB (Page Up and Down on the keyboard).
    if (!tabs.empty() && !choosing) {
        const bool keys = KeyboardPrompts();
        hints.push_back({keys ? "PgUp" : "LB", ""}); does.push_back(3);
        hints.push_back({keys ? "PgDn" : "RB", loc::T("menu.pages")}); does.push_back(4);
    }
    const bool hasStatus = status && *status;
    const int hit = v2::HintBar(hintBar, hints, hasStatus ? status : footerNote,
        hasStatus ? ImGui::ColorConvertFloat4ToU32(ToneColor(statusTone)) : v2::Colors().muted, !statusLeads.empty());
    if (hit >= 0 && !modalAtStart && !noticeOpen) {
        if (hit == static_cast<int>(does.size())) action = {MenuAction::Shortcut, statusLeads};
        else if (does[hit] == 2) { if (choosing) navigation.Cancel(); else backRequested = true; }
        else if (does[hit] == 1 && focusedEntry != entries.end() && !choosing) action = navigation.Choose(entries);
        else if ((does[hit] == 3 || does[hit] == 4) && !modalAtStart) {
            const int next = tab + (does[hit] == 4 ? 1 : -1);
            if (next >= 0 && next < static_cast<int>(tabs.size())) action = {MenuAction::Shortcut, tabs[next].first};
        }
    }
    return Finish(entries, action, backRequested, acceptEditText, static_cast<bool>(body) || valuePanel, noticeOpen);
}
} }
