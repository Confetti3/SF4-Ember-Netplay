// A menu screen as the V2 Overhaul design draws it, cut from the window
// with v2::Box: the hint bar off the bottom, the brand or the title and its
// tabs off the top, then the cards with a detail pane (or, on Home, the
// player's card) beside them. A value's choices open in a panel on its card.
// Input, navigation and the dialogs are GameMenu::Draw's.
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
// A title written in capitals ("SETTINGS").
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
// A card's two portraits (a match's), as tall as its first line's box.
float CardFace(const MenuEntry& e, float height) { return e.Match() ? height - 12 * Scale() : 0; }
bool CardChevron(const MenuEntry& e) { return !e.adjustable && MenuSelectOpens(e) != SelectOpens::Nothing; }
// With `chip` a value that is not a setting stands as an Ember chip (Online play's "B+").
bool CardChip(const MenuEntry& e, bool chip) { return chip && !e.info && !e.value.empty() && !e.adjustable; }
// How a card of `width` and `height` (its one-line box) shares its line
// between the label and the value (FitMenuRow), a chip's width for a value
// that stands as one; a card of a fixed height (`grows` false) keeps it beside.
v2::Share CardShare(const MenuEntry& e, float width, float height, bool chip, bool grows) {
    const float s = Scale(), face = CardFace(e, height);
    const float room = width - 32 * s - (face > 0 ? 2 * (face + 6 * s) : 0);
    if (!CardChip(e, chip)) return FitMenuRow(e, room, 16 * s, grows);
    const float wanted = v2::ChipWidth(e.value) + (CardChevron(e) ? 18 * s : 0);
    return v2::ShareLine(ImGui::CalcTextSize(e.label.c_str()).x, wanted, wanted, room, 16 * s, grows);
}
// One entry's card: its label (and first line of detail on two-line cards), its
// value with the arrows that step it, or a chevron when Select opens something,
// beside the label or under it as `value` shares the card (CardShare).
void DrawCardContent(ImDrawList* d, const MenuEntry& e, v2::Box c, bool enabled, bool twoLines, bool home, v2::Share value, bool chip) {
    const float s = Scale();
    if (e.userText) NoteUserText(e.label);
    const ImU32 text = enabled ? v2::Colors().text : v2::color::Alpha(v2::Colors().text, .55f);
    // A value under its label takes the card's last line, the label and detail the rest.
    const v2::Box line = value.under ? v2::CutBottom(c, v2::UnderLine * s) : c;
    const float h = c.H(), face = CardFace(e, h);
    if (face > 0) {
        DrawCharacterPortrait(e.fighter1, ImVec2(c.min.x + 6 * s, c.min.y + 6 * s), ImVec2(c.min.x + 6 * s + face, c.max.y - 6 * s));
        DrawCharacterPortrait(e.fighter2, ImVec2(c.max.x - 6 * s - face, c.min.y + 6 * s), ImVec2(c.max.x - 6 * s, c.max.y - 6 * s));
    }
    const bool chevron = CardChevron(e);
    chip = CardChip(e, chip);
    const float left = c.min.x + 16 * s + (face > 0 ? face + 6 * s : 0), right = c.max.x - 16 * s - (face > 0 ? face + 6 * s : 0);
    const float labelRoom = right - left - (value.under ? 0 : value.value > 0 ? value.value + 16 * s : chevron ? 18 * s : 0);
    ImFont* labelFont = home ? HeadingFont() : ImGui::GetFont();
    const float labelSize = home ? 20 * s : ImGui::GetFontSize(), detailSize = 15 * s;
    const bool second = twoLines && !e.detail.empty();
    const float labelY = second ? c.min.y + 10 * s : c.min.y + (h - labelSize) * .5f;
    // A player's own words may elide; the probe measures only interface strings.
    d->AddText(labelFont, labelSize, ImVec2(left, labelY), text, Fit(e.label, labelRoom, labelSize, labelFont, !e.userText).c_str());
    if (second) {
        NoteDetailText(e.detail, e.detailText);
        // A card's detail may shorten: the detail pane and speech give it whole.
        d->AddText(ImGui::GetFont(), detailSize, ImVec2(left, labelY + labelSize + 4 * s), v2::Colors().muted, Fit(FirstLine(e.detail), labelRoom, detailSize, ImGui::GetFont(), false).c_str());
    }
    // The value's box at the right end of its line, the chevron's place included.
    const v2::Box box{ImVec2(right - value.value, line.min.y), ImVec2(right, line.max.y)};
    const float end = chevron ? right - 18 * s : right, font = ImGui::GetFontSize();
    if (e.adjustable) {
        DrawMenuValue(d, e, box, enabled, text);
    } else if (chip) {
        v2::Chip(d, ImVec2(end - v2::ChipWidth(e.value), box.min.y + (box.H() - font * .78f - 8 * s) * .5f), e.value, v2::Colors().accent);
    } else if (value.value > 0) {
        if (e.userText || e.text) NoteUserText(e.value);
        // A value may elide (the detail pane says it whole); the probe measures labels.
        const std::string shown = Fit(e.value, end - box.min.x, font, ImGui::GetFont(), false);
        d->AddText(ImVec2(end - ImGui::CalcTextSize(shown.c_str()).x, box.min.y + (box.H() - font) * .5f), e.valueHighlighted ? v2::Colors().accent : v2::Colors().muted, shown.c_str());
    }
    if (chevron) Chevron(d, ImVec2(right - 6 * s, box.min.y + box.H() * .5f), 6 * s, enabled ? v2::Colors().muted : v2::color::Alpha(v2::Colors().muted, .5f));
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
    const std::string fit = Fit(value, box.W() - 40 * s, ImGui::GetFontSize(), ImGui::GetFont(), false);
    const ImU32 arrow = enabled ? col.accent : col.muted;
    d->AddText(ImVec2(box.min.x, y), arrow, "<");
    d->AddText(ImVec2(box.min.x + (box.W() - ImGui::CalcTextSize(fit.c_str()).x) * .5f, y), shown, fit.c_str());
    d->AddText(ImVec2(box.max.x - 10 * s, y), arrow, ">");
}

namespace {
// The value's word now, and what goes round it: the arrows either side of an
// adjustable value, the chevron beside a fixed one.
const char* ValueWord(const MenuEntry& e) { return e.value.empty() ? loc::T("common.not_set") : e.value.c_str(); }
float ValueFrame(const MenuEntry& e) { return (e.adjustable ? 48 : 28) * Scale(); }
}
float MenuValueWidth(const MenuEntry& e) {
    if (MenuValueIsToggle(e)) return 36 * Scale();
    float words = ImGui::CalcTextSize(ValueWord(e)).x;
    for (const auto& c : e.choices) words = (std::max)(words, ImGui::CalcTextSize(c.label.c_str()).x);
    return words + ValueFrame(e);
}
v2::Share FitMenuRow(const MenuEntry& e, float room, float gap, bool grows) {
    if (!e.adjustable && e.value.empty()) return {};
    // What the value gives way down to depends on the row, not on the word it
    // shows now, so stepping a value or a status changing never moves the rows:
    // a control (arrows, a switch) to under half the row, a fixed value to its
    // chevron's place (cut; the detail pane says it whole).
    const float wanted = MenuValueWidth(e);
    const float least = (std::min)(wanted, e.adjustable ? room * .45f : ValueFrame(e));
    return v2::ShareLine(ImGui::CalcTextSize(e.label.c_str()).x, wanted, least, room, gap, grows);
}
// Where a click on a value steps it: a number's either half, a word's arrows
// only (the word itself opens its list), a switch's own box (which flips it,
// so a switch that also reads is flipped on the switch and read on its label).
static int MenuValueStep(const MenuEntry& e, float mx, float vx, float width) {
    if (!e.adjustable || !e.enabled || width <= 0 || mx < vx) return 0;
    if (MenuValueIsToggle(e)) return mx < vx + width - 36 * Scale() ? 0 : e.value == loc::T("common.on") ? -1 : 1;
    if (!MenuValueIsNumber(e) && mx >= vx + 24 * Scale() && mx <= vx + width - 24 * Scale()) return 0;
    return mx < vx + width * .5f ? -1 : 1;
}
void DrawMenuRow(ImDrawList* d, const MenuEntry& e, v2::Box r, bool enabled, v2::Share value, float inset) {
    const float s = Scale(), font = ImGui::GetFontSize(), left = r.min.x + inset, right = r.max.x - inset;
    const auto& c = v2::Colors();
    const ImU32 text = enabled ? c.text : c.muted;
    // A value under its label takes the row's last line, the label the rest.
    const v2::Box line = value.under ? v2::CutBottom(r, v2::UnderLine * s) : r;
    // Beside it the label leaves the value its share, or a chevron its place.
    const float beside = value.under ? 0 : value.value > 0 ? value.value + 8 * s : e.info ? 0 : 18 * s;
    v2::Text(d, ImVec2(left, r.min.y + (r.H() - font) * .5f), right - left - beside, e.label, font, text, nullptr, false, !e.userText);
    if (e.userText || e.text) NoteUserText(e.value);
    const v2::Box box{ImVec2(right - value.value, line.min.y), ImVec2(right, line.max.y)};
    if (e.adjustable) { DrawMenuValue(d, e, box, enabled, text); return; }
    // A fixed value, then the chevron of a row Select opens (an info row has none).
    const float end = e.info ? right : right - 18 * s;
    if (!e.value.empty()) {
        const std::string shown = Fit(e.value, value.value - 20 * s, font, ImGui::GetFont(), false);
        d->AddText(ImVec2(end - ImGui::CalcTextSize(shown.c_str()).x, box.min.y + (box.H() - font) * .5f), e.valueHighlighted ? c.accent : e.info ? c.muted : text, shown.c_str());
    }
    if (!e.info) Chevron(d, ImVec2(right - 6 * s, box.min.y + box.H() * .5f), 6 * s, enabled ? c.muted : v2::color::Alpha(c.muted, .5f));
}
EntryHit PressEntry(MenuNavigation& nav, const std::vector<MenuEntry>& rows, const MenuEntry& e, v2::Box b, MenuAction& action,
    bool scroll, float valueWidth, bool choose) {
    // Directions move between a board's rows by where they are drawn.
    nav.Place(e.id, {b.min.x, b.min.y, b.max.x, b.max.y});
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
        const int step = MenuValueStep(e, mx, vx, valueWidth);
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
    if (backdrop && !flyout) DrawMenuBackdrop();
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
        // A short window (TV size, 150% at 1080p) takes the small mark, so Home's rows keep the height.
        const float mark = (narrow || content.H() < 760 * s ? 44 : 72) * s;
        Brand(d, content.min, mark);
        v2::CutTop(content, mark, 24 * s);
    } else {
        ImGui::BeginGroup();
        // A title in capitals gives way to the screen's name as the menus write it.
        // A flyout's root is named by its owner (rootName), its pages as the menus name them.
        const char* named = navigation.Screen() == navigation.Root() && !rootName.empty() ? rootName.c_str() : MenuScreenName(navigation.Screen());
        // Its Back button is the hint bar's Back, so it closes an open question first.
        if (v2::Header(named && IsCapitals(title) ? named : title, eyebrow.empty() ? nullptr : eyebrow.c_str(), headerChips, backInert ? nullptr : loc::T("common.back"))
            && !noticeOpen && !navigation.Editing()) {
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
        if (valuesClose_ && navigation.Choosing()) navigation.Cancel();
        valuesClose_ = false;
        if (!navigation.Choosing() || focusedEntry == entries.end() || focusCard.W() <= 0 || !notice_.empty()) { valuesOpen_ = false; return false; }
        const auto& e = *focusedEntry;
        // Select on a switch flips it: no popup for two words.
        if (MenuValueIsToggle(e)) { navigation.Cancel(); action = {MenuAction::Adjust, e.id, {}, e.value == loc::T("common.on") ? -1 : 1}; return false; }
        const auto grid = MenuChoiceLayout(e.choices);
        // The click that opened it is not a click outside it.
        const bool opening = !valuesOpen_;
        valuesOpen_ = true;
        const std::size_t count = e.choices.size(), across = e.popout ? 1 : grid.across, lines = (count + across - 1) / across;
        const float row = (e.popout ? 44 : 48) * s, inner = (e.popout ? 8 : 6) * s, foot = e.popout ? 0 : 30 * s;
        // A value panel's head: its title (the row's label) and the count of its
        // choices, sharing the line as a row does (v2::ShareLine) in `room`.
        const std::string counted = loc::Tf("menu.choices_count", count);
        const float titleWidth = HeadingFont()->CalcTextSizeA(18 * s, FLT_MAX, 0, e.label.c_str()).x, countWidth = ImGui::CalcTextSize(counted.c_str()).x;
        const auto head = [&](float room) { return v2::ShareLine(titleWidth, countWidth, countWidth, room - 8 * s, 16 * s); };
        // As wide as its choices, the keys under them and its head ask, inside the column.
        const auto chosen = std::find_if(e.choices.begin(), e.choices.end(), [&](const MenuChoice& o) { return o.id == e.chosen; });
        const std::string keysLine = loc::Tf("menu.choices_keys", chosen == e.choices.end() ? e.value : chosen->label);
        const float keys = ImGui::CalcTextSize(keysLine.c_str()).x;
        const float panelWidth = (std::min)(list.W(), (std::max)({keys, across * grid.cell + (across - 1) * inner, titleWidth + countWidth + 24 * s}) + 24 * s);
        const float headLine = !e.popout && head(panelWidth - 24 * s).under ? v2::UnderLine * s : 0;
        const float height = lines * row + (lines - 1) * inner + 24 * s + (e.popout ? 22 : 38) * s + headLine + foot;
        if (e.popout) {
            // A popout opens where it was asked for: at the click, or beside the row for the pad
            // and keys, and turns back at the screen's edges.
            if (opening) popoutAt_ = ImGui::IsMouseReleased(ImGuiMouseButton_Left) ? ImGui::GetMousePos() : ImVec2(focusCard.max.x - 24 * s, focusCard.min.y);
            const float w = 300 * s, h = (std::min)(height, size.y - 16 * s);
            ImVec2 at = popoutAt_;
            if (at.x + w > win.x + size.x - 8 * s) at.x = (std::max)(win.x + 8 * s, at.x - w);
            if (at.y + h > win.y + size.y - 8 * s) at.y = (std::max)(win.y + 8 * s, win.y + size.y - 8 * s - h);
            ImGui::SetNextWindowPos(at);
            ImGui::SetNextWindowSize(ImVec2(w, h));
        } else {
            // At the column's right edge, level with the row, kept inside the column.
            const float top = (std::max)(list.min.y, (std::min)(focusCard.min.y, list.max.y - height));
            ImGui::SetNextWindowPos(ImVec2(list.max.x - panelWidth - 12 * s, top));
            ImGui::SetNextWindowSize(ImVec2(panelWidth, (std::min)(height, list.H())));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * s, 12 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4 * s);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(v2::Colors().panel));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(v2::Colors().accent));
        // A list taller than the column scrolls, with a scrollbar so its last choices are seen to be there.
        ImGui::Begin("###V2 values", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
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
            const v2::Share share = head(avail);
            const float under = share.under ? v2::UnderLine * s : 0;
            v2::Text(pd, ImVec2(at.x + 4 * s, at.y), avail - 8 * s - (share.under ? 0 : countWidth + 16 * s), e.label, 18 * s, col.text, HeadingFont());
            pd->AddText(ImVec2(at.x + avail - countWidth - 4 * s, at.y + 2 * s + under), col.muted, counted.c_str());
            pd->AddLine(ImVec2(at.x, at.y + 28 * s + under), ImVec2(at.x + avail, at.y + 28 * s + under), col.border, s);
            ImGui::Dummy(ImVec2(0, 30 * s + under));
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
        // A click outside an open popout or value panel closes it. The board under it was drawn
        // while it was still open, so that click does nothing else.
        // It closes on the next frame, after the hint bar's Back has had this click.
        valuesClose_ = !opening && navigation.Choosing() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered();
        ImGui::PopStyleVar();
        if (!e.popout && navigation.Choosing()) {
            ImGui::Dummy(ImVec2(0, 2 * s));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col.muted), "%s", keysLine.c_str());
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
        // Home's rows share the list's height, from two lines (64 px) down to
        // one (52), so all of them show without a scroll where they can.
        float homeRow = 64 * s;
        if (home) {
            const float rows = static_cast<float>(std::count_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return !(split && e.id == "awards"); }));
            if (rows > 0) homeRow = (std::max)(52 * s, (std::min)(64 * s, (list.H() - 8 * s * (rows - 1)) / rows));
        }
        // A flyout keeps to one line a card past its first page, so more of its settings show at once.
        const bool twoLines = home ? homeRow >= 60 * s : listWidth > 0 || (!split && (!flyout || navigation.Screen() == navigation.Root())), grid = columns > 1 && card;
        const std::size_t cells = grid ? MenuGridCells(entries) : 0;
        const float gap = 8 * s;
        // A card's one-line box, and how its label and value share it at `width`:
        // a card of the list's own height grows by a line for a value under its label.
        const auto lineOf = [&](const MenuEntry& e, bool cell) {
            return cell ? cardHeight * s : e.height > 0 ? e.height * s : home ? homeRow : (twoLines && !e.detail.empty() ? listWidth > 0 ? 68 : 64 : 52) * s;
        };
        const auto shareOf = [&](const MenuEntry& e, bool cell, float width) {
            return CardShare(e, width, lineOf(e, cell), listWidth > 0, !cell && !home && e.height <= 0);
        };
        const auto heightOf = [&](const MenuEntry& e, bool cell, float width) {
            return lineOf(e, cell) + (shareOf(e, cell, width).under ? v2::UnderLine * s : 0);
        };
        const auto heads = [&](std::size_t i) { return !entries[i].group.empty() && (i == 0 || entries[i - 1].group != entries[i].group); };
        // A flyout whose cards do not fit its height (Training's pages at 150%)
        // shows them in two columns where they still have room, the first half
        // down the left: still one list in reading order, so Down from the
        // foot of the left column goes on to the top of the right.
        float stacked = 0;
        for (std::size_t i = 0; i < entries.size() && !grid; ++i) stacked += heightOf(entries[i], false, list.W()) + gap + (heads(i) ? 26 * s + gap : 0);
        const bool halves = flyout && !grid && stacked > list.H() && list.W() >= 520 * s;
        const std::size_t half = halves ? (entries.size() + 1) / 2 : entries.size();
        v2::Box focusCard{};
        ImGui::SetCursorScreenPos(list.min);
        if (changed) ImGui::SetNextWindowScroll(ImVec2(0, navigation.Scroll()));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("V2 list", ImVec2(list.W(), list.H()), 0, ImGuiWindowFlags_NoNavInputs);
        auto* ld = ImGui::GetWindowDrawList();
        const float full = halves ? (ImGui::GetContentRegionAvail().x - gap) * .5f : ImGui::GetContentRegionAvail().x;
        const float cellWidth = grid ? (full - gap * (columns - 1)) / columns : full;
        const ImVec2 top = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i == half && halves) ImGui::SetCursorScreenPos(ImVec2(top.x + full + gap, top.y));
            const auto& e = entries[i];
            // Home's "awards" row is the player's card in the pane.
            if (home && split && e.id == "awards") continue;
            const bool focused = e.id == navigation.Focus(), enabled = feedback_.Enabled(e), cell = i < cells;
            if (cell && i % columns) ImGui::SameLine(0, gap);
            // A group's heading over its first card.
            if (!cell && heads(i)) {
                if (i && i != half) ImGui::Dummy(ImVec2(0, 8 * s));
                const auto at = ImGui::GetCursorScreenPos();
                v2::SectionHeading(ld, ImVec2(at.x + 2 * s, at.y + 4 * s), e.group);
                ImGui::Dummy(ImVec2(full, 18 * s));
            }
            const float w = cell ? cellWidth : full;
            const v2::Share value = shareOf(e, cell, w);
            const float h = lineOf(e, cell) + (value.under ? v2::UnderLine * s : 0);
            const auto p = ImGui::GetCursorScreenPos();
            const v2::Box c{p, ImVec2(p.x + w, p.y + h)};
            ImGui::PushID(e.id.c_str());
            const bool pressed = ImGui::InvisibleButton("##card", ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            ReportMenuCard(e.id.c_str(), c.min, c.max);
            // The value's ends step it; anything else on the card is Select.
            const float valueWidth = e.adjustable ? value.value : 0;
            if (pressed && listEnabled && !modalAtStart && !navigation.Editing() && !navigation.Asking()) {
                navigation.Focus(e.id, entries);
                const float mx = ImGui::GetMousePos().x, vx = c.max.x - 16 * s - valueWidth;
                const int step = MenuValueStep(e, mx, vx, valueWidth);
                action = step ? MenuAction{MenuAction::Adjust, e.id, {}, step} : navigation.Choose(entries);
            } else if (listEnabled && pointerMoved && hovered && !navigation.Choosing()) navigation.Focus(e.id, entries);
            if (focused) {
                focusCard = c;
                if (lastFocus_ != e.id || changed) ImGui::SetScrollHereY(.5f);
            }
            if (!ImGui::IsItemVisible()) continue;
            v2::CardGround(ld, c, focused, hovered, e.valueHighlighted);
            // A screen's own drawing of its rows (portraits, flags, rankings).
            if (card && card(e, c.min, c.max)) { if (focused) v2::Focus(ld, c.min, c.max, !cell); continue; }
            DrawCardContent(ld, e, c, enabled, twoLines && !cell, home, value, listWidth > 0);
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
                    d->AddText(HeadingFont(), 16 * s, ImVec2(box.min.x + 16 * s, y + 10 * s), c.text, Fit(news[i].title, box.W() - 32 * s, 16 * s, HeadingFont(), false).c_str());
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
    // The right mouse button is Back, as B and Escape: it closes an open value
    // panel or question first, and answers a focused toast as B does.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !noticeOpen && !navigation.Editing()) {
        if (toastFocused) toastPressed |= MenuInput::Back;
        else if (navigation.Asking()) navigation.Cancel();
        else if (!backInert) backRequested = true;
    }

    // The hint bar: what the buttons do now, each also a button that presses
    // it; the status on the right. An open dialog names its own Select and
    // Back (DialogLegend); a list that takes no input (a capture) shows only
    // the screen's own hints.
    std::vector<v2::Hint> hints;
    std::vector<unsigned> presses;
    const auto hint = [&](const char* glyph, std::string label, unsigned press) { hints.push_back({glyph, std::move(label)}); presses.push_back(press); };
    std::string dialogSelect, dialogBack;
    const bool dialog = DialogLegend(entries, dialogSelect, dialogBack);
    if (dialog) {
        if (!dialogSelect.empty()) hint(MenuSelectGlyph(), dialogSelect, MenuInput::Select);
        hint(MenuBackGlyph(), dialogBack, MenuInput::Back);
    } else if (listEnabled) {
        MenuEntry shown;
        if (focusedEntry != entries.end()) { shown = *focusedEntry; shown.enabled = feedback_.Enabled(*focusedEntry); }
        if (const char* primary = MenuPrimaryHint(focusedEntry == entries.end() ? nullptr : &shown)) hint(MenuSelectGlyph(), primary, MenuInput::Select);
        if (!backInert) hint(MenuBackGlyph(), backHint.empty() ? loc::T("common.back") : backHint, MenuInput::Back);
        if (shown.adjustable && !MenuValueIsToggle(shown) && shown.enabled) hint(KeyboardPrompts() ? "keys-horizontal" : "horizontal", loc::T("menu.adjust"), MenuInput::Right);
    }
    for (const auto& h : shortcutHints) hint(h.glyph, h.label, h.press);
    // Tabs step with the device's tab buttons (LB and RB, Page Up and Down).
    if (!tabs.empty() && !dialog) {
        const char* previous = MenuShortcutGlyph(MenuInput::PrevTab);
        const char* next = MenuShortcutGlyph(MenuInput::NextTab);
        if (previous) hint(previous, next ? "" : loc::T("menu.pages"), MenuInput::PrevTab);
        if (next) hint(next, loc::T("menu.pages"), MenuInput::NextTab);
    }
    const bool hasStatus = status && *status;
    const int hit = v2::HintBar(hintBar, hints, hasStatus ? status : footerNote,
        hasStatus ? ImGui::ColorConvertFloat4ToU32(ToneColor(statusTone)) : v2::Colors().muted, !statusLeads.empty());
    // A hint pressed is its button pressed.
    if (hit >= 0 && !noticeOpen && !navigation.Editing()) {
        const unsigned press = hit < static_cast<int>(presses.size()) ? presses[hit] : 0;
        if (hit == static_cast<int>(presses.size())) action = {MenuAction::Shortcut, statusLeads};
        else if (press == MenuInput::Back) { if (navigation.Asking()) navigation.Cancel(); else backRequested = true; }
        else if (press == MenuInput::Select) action = navigation.Choosing() ? navigation.Pick(navigation.ChoiceIndex(), entries) : navigation.Choose(entries);
        else if (press == MenuInput::Right && focusedEntry != entries.end()) action = {MenuAction::Adjust, focusedEntry->id, {}, 1};
        else if (press) {
            action = {MenuAction::Shortcut, {}, {}, static_cast<int>(press)};
            if (const int step = TabDelta(action); step && !tabs.empty()) action = TabAction(step);
        }
    }
    // The board is its whole window, edges included (its lists and bodies scroll
    // in their own children): the hint bar on the bottom edge, past the window's
    // padding, is no content for the window to scroll to.
    auto* window = ImGui::GetCurrentWindow();
    window->DC.CursorMaxPos.y = (std::min)(window->DC.CursorMaxPos.y, window->DC.CursorStartPos.y + window->Size.y - 2 * window->WindowPadding.y);
    return Finish(entries, action, backRequested, acceptEditText, static_cast<bool>(body) || valuePanel, noticeOpen);
}
} }
