// A menu screen as Ember draws it (the design canvas' boards), cut from the
// window with v2::Box: the hint bar off the bottom, the brand or the title and
// its tabs off the top, then the cards with a detail pane (or, on Home, the
// player's card) beside them, or a screen's own body. A value's choices open
// in a panel on its card. GameMenu::ReadFrame (GameMenu.cxx) reads the input
// first; the dialogs come last (Finish).
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
#include <iterator>

namespace sf4e { namespace ui {
namespace {
using v2::Fit;
using v2::Chevron;
v2::Box BoxOf(const MenuRect& r) { return {ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1)}; }
MenuRect RectOf(v2::Box b) { return {b.min.x, b.min.y, b.max.x, b.max.y}; }
const MenuEntry* FocusedIn(const std::vector<MenuEntry>& entries, const std::string& id) {
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == id; });
    return it == entries.end() ? nullptr : &*it;
}
bool PointerMoved() { const auto mouse = ImGui::GetIO().MouseDelta; return mouse.x != 0 || mouse.y != 0; }
// A title written in capitals ("SETTINGS").
bool IsCapitals(const char* text) {
    bool letter = false;
    for (; *text; ++text) { if (*text >= 'a' && *text <= 'z') return false; letter |= *text >= 'A' && *text <= 'Z'; }
    return letter;
}
std::string FirstLine(const std::string& text) { return text.substr(0, text.find('\n')); }
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
// How many lines a row's label takes across `room`, the width it is drawn in
// (MenuRowLayout::labelLines): where its value has already gone under it, a
// label wider than its whole line wraps to a second (v2::Heading's whole
// words). A player's own words keep to one line and may elide, and so does a
// match's label, whose portraits stand as tall as the card (CardLayout).
int LabelLines(const MenuEntry& e, v2::Share value, float room) {
    const float size = ImGui::GetFontSize();
    return value.under && !e.userText && !e.Match() && v2::HeadingHeight(e.label, room, size, ImGui::GetFont()) > size ? 2 : 1;
}
// A row's label with its top at `at`, `width` wide, on the lines its layout
// gave it: two through v2::Heading, else one fitted (the probe measures only
// interface strings). Returns its height.
float DrawRowLabel(ImDrawList* d, const MenuEntry& e, int lines, ImVec2 at, float width, float size, ImFont* font, ImU32 color) {
    if (lines > 1) return v2::Heading(d, at, width, e.label, size, color, font);
    v2::Text(d, at, width, e.label, size, color, font, false, !e.userText);
    return size;
}
float RowLabelHeight(int lines, float size) { return lines > 1 ? size * (1 + v2::HeadingLine) : size; }
// A card whose one-line box is `line`, laid out as `value` shares it: its
// words 16 px in, past the portraits of a match (as tall as the card with its
// value under; a match's label never wraps, LabelLines).
MenuRowLayout CardLayout(const MenuEntry& e, v2::Box line, v2::Share value) {
    const float s = Scale(), face = CardFace(e, line.H() + (value.under ? v2::UnderLine * s : 0)), beyond = face > 0 ? face + 6 * s : 0;
    const float left = line.min.x + 16 * s + beyond, right = line.max.x - 16 * s - beyond;
    return LayOutMenuRow(RectOf(line), left, right, value.value, value.under, v2::UnderLine * s, 16 * s, LabelLines(e, value, right - left));
}
// One entry's card: its label (and first line of detail on two-line cards), its
// value with the arrows that step it, or a chevron when Select opens something,
// where `at` (CardLayout) puts them.
void DrawCardContent(ImDrawList* d, const MenuEntry& e, v2::Box c, const MenuRowLayout& at, bool enabled, bool twoLines, bool home, bool chip) {
    const float s = Scale();
    if (e.userText) NoteUserText(e.label);
    const ImU32 text = enabled ? v2::Colors().text : v2::color::Alpha(v2::Colors().text, .55f);
    const float face = CardFace(e, c.H());
    if (face > 0) {
        DrawCharacterPortrait(e.fighter1, ImVec2(c.min.x + 6 * s, c.min.y + 6 * s), ImVec2(c.min.x + 6 * s + face, c.max.y - 6 * s));
        DrawCharacterPortrait(e.fighter2, ImVec2(c.max.x - 6 * s - face, c.min.y + 6 * s), ImVec2(c.max.x - 6 * s, c.max.y - 6 * s));
    }
    const bool chevron = CardChevron(e);
    chip = CardChip(e, chip);
    const v2::Box label = BoxOf(at.label), box = BoxOf(at.value);
    const float labelRoom = label.W() - (!at.under && box.W() <= 0 && chevron ? 18 * s : 0);
    ImFont* labelFont = home ? HeadingFont() : ImGui::GetFont();
    const float labelSize = home ? 20 * s : ImGui::GetFontSize(), detailSize = 15 * s;
    const bool second = twoLines && !e.detail.empty();
    const float labelY = second ? c.min.y + 10 * s : label.min.y + (label.H() - RowLabelHeight(at.labelLines, labelSize)) * .5f;
    const float labelHeight = DrawRowLabel(d, e, at.labelLines, ImVec2(label.min.x, labelY), labelRoom, labelSize, labelFont, text);
    if (second) {
        NoteDetailText(e.detail, e.detailText);
        // A card's detail may shorten: the detail pane and speech give it whole.
        d->AddText(ImGui::GetFont(), detailSize, ImVec2(label.min.x, labelY + labelHeight + 4 * s), v2::Colors().muted, Fit(FirstLine(e.detail), labelRoom, detailSize, ImGui::GetFont(), false).c_str());
    }
    // The value's box at the right end of its line, the chevron's place included.
    const float right = box.max.x, end = chevron ? right - 18 * s : right, font = ImGui::GetFontSize();
    if (e.adjustable) {
        DrawMenuValue(d, e, box, enabled, text);
    } else if (chip) {
        v2::Chip(d, ImVec2(end - v2::ChipWidth(e.value), box.min.y + (box.H() - font * .78f - 8 * s) * .5f), e.value, v2::Colors().accent);
    } else if (box.W() > 0) {
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
// What a press under the pointer does to the value laid out `at` (MenuValueStep):
// a switch's own 36 px flips it, a word's 24 px arrows step it.
int StepAtPointer(const MenuEntry& e, const MenuRowLayout& at) {
    const auto mouse = ImGui::GetMousePos();
    const int toggle = MenuValueIsToggle(e) ? (e.value == loc::T("common.on") ? -1 : 1) : 0;
    return MenuValueStep(e, at, mouse.x, mouse.y, toggle, 36 * Scale(), 24 * Scale());
}
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
MenuRowLayout LayOutBoardRow(const MenuEntry& e, ImVec2 at, float width) {
    const float s = Scale(), inset = 14 * s, room = width - 2 * inset;
    const v2::Share value = FitMenuRow(e, room, 8 * s);
    return LayOutMenuRow({at.x, at.y, at.x + width, at.y + 48 * s}, at.x + inset, at.x + width - inset, value.value, value.under, v2::UnderLine * s, 8 * s,
        LabelLines(e, value, room));
}
void DrawMenuRow(ImDrawList* d, const MenuEntry& e, const MenuRowLayout& at, bool enabled) {
    const float s = Scale(), font = ImGui::GetFontSize();
    const auto& c = v2::Colors();
    const ImU32 text = enabled ? c.text : c.muted;
    const v2::Box label = BoxOf(at.label), box = BoxOf(at.value);
    // With no value beside it the label leaves the chevron its place (an info row has none).
    const float chevron = !at.under && box.W() <= 0 && !e.info ? 18 * s : 0;
    DrawRowLabel(d, e, at.labelLines, ImVec2(label.min.x, label.min.y + (label.H() - RowLabelHeight(at.labelLines, font)) * .5f), label.W() - chevron, font, ImGui::GetFont(), text);
    if (e.userText || e.text) NoteUserText(e.value);
    if (e.adjustable) { DrawMenuValue(d, e, box, enabled, text); return; }
    // A fixed value, then the chevron of a row Select opens (an info row has none).
    const float right = box.max.x, end = e.info ? right : right - 18 * s;
    if (!e.value.empty()) {
        const std::string shown = Fit(e.value, box.W() - 20 * s, font, ImGui::GetFont(), false);
        d->AddText(ImVec2(end - ImGui::CalcTextSize(shown.c_str()).x, box.min.y + (box.H() - font) * .5f), e.valueHighlighted ? c.accent : e.info ? c.muted : text, shown.c_str());
    }
    if (!e.info) Chevron(d, ImVec2(right - 6 * s, box.min.y + box.H() * .5f), 6 * s, enabled ? c.muted : v2::color::Alpha(c.muted, .5f));
}
EntryHit PressEntry(MenuNavigation& nav, const std::vector<MenuEntry>& rows, const MenuEntry& e, v2::Box b, MenuAction& action,
    bool scroll, const MenuRowLayout& layout, bool choose) {
    // Directions move between a board's rows by where they are drawn.
    nav.Place(e.id, RectOf(b));
    ImGui::SetCursorScreenPos(b.min);
    ImGui::PushID(e.id.c_str());
    const bool pressed = ImGui::InvisibleButton("##entry", ImVec2(b.W(), b.H())) && !nav.Asking() && !nav.Editing();
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (scroll && e.id == nav.Focus()) ImGui::SetScrollHereY(.5f);
    if (pressed && choose) {
        nav.Focus(e.id, rows);
        // The value steps where it is drawn; anything else on the row is Select.
        const int step = StepAtPointer(e, layout);
        action = step ? MenuAction{MenuAction::Adjust, e.id, {}, step} : nav.Choose(rows);
    } else if (hovered && PointerMoved() && !nav.Choosing()) nav.Focus(e.id, rows);
    return {pressed, hovered};
}
float BoardRowHeight(const MenuEntry& e, float width) {
    const MenuRect row = LayOutBoardRow(e, ImVec2(0, 0), width).row;
    return row.y1 - row.y0;
}
BoardRowHit GameMenu::DrawBoardRow(const std::vector<MenuEntry>& rows, const MenuEntry& e, ImVec2 at, float width, MenuAction& action, const BoardRowStyle& style) {
    const MenuRowLayout layout = LayOutBoardRow(e, at, width);
    const v2::Box row = BoxOf(layout.row);
    const EntryHit hit = PressEntry(navigation, rows, e, row, action, style.scroll, layout, style.choose);
    const bool focused = style.choose && e.id == navigation.Focus(), hovered = style.choose && hit.hovered;
    auto* d = ImGui::GetWindowDrawList();
    if (style.card) v2::CardGround(d, row, focused, hovered);
    else v2::RowGround(d, row, focused, hovered);
    DrawMenuRow(d, e, layout, style.enabled);
    if (focused) {
        v2::Focus(d, row.min, row.max);
        if (!e.choices.empty()) bodyChoices = {row, style.column};
    }
    return {row, hit};
}

MenuAction GameMenu::Draw(const char* title, const std::vector<MenuEntry>& entries, const char* status, const Detail& detail, int columns,
    const Card& card, const Body& body, bool flyout, float cardHeight, Tone statusTone, bool home) {
    Frame f{title, entries, status, detail, (std::max)(1, columns), card, body, flyout, cardHeight, statusTone, home};
    ReadFrame(f);
    const float s = Scale();
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
    const bool narrow = v2::Narrow(size.x, s);
    const float side = flyout ? 16 * s : v2::SideMargin(size.x, s), pad = flyout ? 12 * s : v2::Pad(size.x, s);
    // A flyout (the training controls) sits over the fight: no art behind it.
    if (backdrop && !flyout) DrawMenuBackdrop();
    v2::Box screen{win, ImVec2(win.x + size.x, win.y + size.y)};
    const v2::Box hintBar = v2::CutBottom(screen, (narrow || flyout ? 48 : 56) * s, (flyout ? 8 : 16) * s);
    v2::Box content{ImVec2(screen.min.x + side, screen.min.y + pad), ImVec2(screen.max.x - side, screen.max.y)};
    DrawHeader(f, content, narrow);
    if (body && !navigation.Reading()) DrawBody(f, content);
    else if (navigation.Reading()) {
        ImGui::SetCursorScreenPos(content.min);
        DrawReader(entries, detail, content.H(), f.held);
    } else DrawCards(f, content, narrow);
    if (f.action.kind == MenuAction::Back) { f.backRequested = true; f.action = {}; }
    // The right mouse button is Back, as B and Escape: it closes an open value
    // panel or question first, and answers a focused toast as B does.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !f.noticeOpen && !navigation.Editing()) {
        if (toastFocused) toastPressed |= MenuInput::Back;
        else if (navigation.Asking()) navigation.Cancel();
        else if (!backInert) f.backRequested = true;
    }
    DrawHintBar(f, hintBar);
    return Finish(f);
}

void GameMenu::DrawHeader(Frame& f, v2::Box& content, bool narrow) {
    const float s = Scale();
    ImGui::SetCursorScreenPos(content.min);
    if (f.home) {
        // A short window (TV size, 150% at 1080p) takes the small mark, so Home's rows keep the height.
        const float mark = (narrow || content.H() < 760 * s ? 44 : 72) * s;
        Brand(ImGui::GetWindowDrawList(), content.min, mark);
        v2::CutTop(content, mark, 24 * s);
        return;
    }
    ImGui::BeginGroup();
    // A title in capitals gives way to the screen's name as the menus write it.
    // A flyout's root is named by its owner (rootName), its pages as the menus name them.
    const char* named = navigation.Screen() == navigation.Root() && !rootName.empty() ? rootName.c_str() : MenuScreenName(navigation.Screen());
    // Its Back button is the hint bar's Back, so it closes an open question first.
    if (v2::Header(named && IsCapitals(f.title) ? named : f.title, eyebrow.empty() ? nullptr : eyebrow.c_str(), headerChips, backInert ? nullptr : loc::T("common.back"))
        && !f.noticeOpen && !navigation.Editing()) {
        if (navigation.Asking()) navigation.Cancel();
        else f.backRequested = true;
    }
    if (!tabs.empty()) {
        std::vector<std::string> labels;
        for (const auto& t : tabs) labels.push_back(t.second);
        const ImVec2 strip = ImGui::GetCursorScreenPos();
        const float stripWidth = ImGui::GetContentRegionAvail().x;
        const int hit = v2::Tabs(labels, tab);
        // The strip as the probes find it, its step marks at its ends.
        ReportMenuCard("tabs", strip, ImVec2(strip.x + stripWidth, ImGui::GetCursorScreenPos().y));
        if (hit >= 0 && hit != tab && !f.modalAtStart) f.action = {MenuAction::Shortcut, tabs[hit].first};
    }
    ImGui::EndGroup();
    content.min.y = ImGui::GetCursorScreenPos().y;
}

void GameMenu::DrawBody(Frame& f, v2::Box content) {
    const float s = Scale(), card = 64 * s, gap = 8 * s;
    // The shell's own rows (MenuEntry::footer) are never the body's: they
    // stand under it as cards of two lines, their label and why, in a band of
    // their own, placed (PressEntry) so the pad reaches them from the body's
    // rows by where they are drawn.
    std::vector<const MenuEntry*> footer;
    std::vector<MenuEntry> own;
    for (const auto& e : f.entries) if (e.footer) footer.push_back(&e);
    if (!footer.empty()) std::copy_if(f.entries.begin(), f.entries.end(), std::back_inserter(own), [](const MenuEntry& e) { return !e.footer; });
    const v2::Box band = footer.empty() ? v2::Box{} : v2::CutBottom(content, footer.size() * (card + gap) - gap, 12 * s);
    const bool inert = (f.modalAtStart && !f.choosingAtStart) || navigation.Editing() || navigation.Confirming();
    ImGui::SetCursorScreenPos(content.min);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("Body", ImVec2(content.W(), content.H()), 0, ImGuiWindowFlags_NoNavInputs);
    ImGui::BeginDisabled(inert);
    bodyChoices = {};
    f.body(footer.empty() ? f.entries : own, navigation, f.action, content.H(), feedback_);
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(inert);
    auto* d = ImGui::GetWindowDrawList();
    float y = band.min.y;
    for (const MenuEntry* e : footer) {
        const v2::Box box{ImVec2(band.min.x, y), ImVec2(band.max.x, y + card)};
        y = box.max.y + gap;
        const MenuRowLayout layout = CardLayout(*e, box, CardShare(*e, box.W(), card, false, false));
        ReportMenuCard(e->id.c_str(), box.min, box.max);
        const bool hovered = PressEntry(navigation, f.entries, *e, box, f.action, false, layout).hovered, focused = e->id == navigation.Focus();
        v2::CardGround(d, box, focused, hovered);
        DrawCardContent(d, *e, box, layout, feedback_.Enabled(*e), true, false, false);
        if (focused) v2::Focus(d, box.min, box.max);
    }
    ImGui::EndDisabled();
    f.valuePanel = DrawValuePanel(f, bodyChoices.card, bodyChoices.column);
}

void GameMenu::DrawCards(Frame& f, v2::Box content, bool narrow) {
    const float s = Scale();
    // Home: the cards and a column with the player. Elsewhere: the cards and the focused one's detail.
    const bool split = !narrow && content.W() >= 820 * s;
    v2::Box list = content, pane{};
    if (split) {
        const float w = f.home ? (std::min)(460 * s, content.W() * .42f) : listWidth > 0 ? content.W() - (std::min)(listWidth * s, content.W() * .55f) - 24 * s :
            paneWidth > 0 ? (std::min)(paneWidth * s, content.W() * .45f) : content.W() * .45f - 24 * s;
        pane = paneLeft ? v2::CutLeft(list, w, 24 * s) : v2::CutRight(list, w, 24 * s);
    }
    if (f.home && split) list.max.x = list.min.x + (std::min)(600 * s, list.W());
    const v2::Box focusCard = DrawCardList(f, list, split);
    if (split && f.home) DrawHomePane(f, pane);
    else if (split && FocusedIn(f.entries, navigation.Focus())) DrawDetailPane(f, pane);
    f.valuePanel = DrawValuePanel(f, focusCard, list);
    if (listCard.draw) DrawListCard(list.min, list.max, focusCard.min, focusCard.max, s);
}

v2::Box GameMenu::DrawCardList(Frame& f, v2::Box list, bool split) {
    const float s = Scale();
    const auto& entries = f.entries;
    const bool home = f.home, changed = lastScreen_ != navigation.Screen(), pointerMoved = PointerMoved();
    // Home's rows share the list's height, from two lines (64 px) down to
    // one (52), so all of them show without a scroll where they can.
    float homeRow = 64 * s;
    if (home) {
        const float rows = static_cast<float>(std::count_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return !(split && e.id == "awards"); }));
        if (rows > 0) homeRow = (std::max)(52 * s, (std::min)(64 * s, (list.H() - 8 * s * (rows - 1)) / rows));
    }
    // A flyout keeps to one line a card past its first page, so more of its settings show at once.
    const bool twoLines = home ? homeRow >= 60 * s : listWidth > 0 || (!split && (!f.flyout || navigation.Screen() == navigation.Root()));
    const bool grid = f.columns > 1 && f.card;
    const std::size_t cells = grid ? MenuGridCells(entries) : 0;
    const float gap = 8 * s;
    // A card's one-line box, and its layout at `at`, `width` wide: a card of
    // the list's own height grows by a line for a value under its label, and
    // by another for a label wider than its whole line (CardLayout).
    const auto lineOf = [&](const MenuEntry& e, bool cell) {
        return cell ? f.cardHeight * s : e.height > 0 ? e.height * s : home ? homeRow : (twoLines && !e.detail.empty() ? listWidth > 0 ? 68 : 64 : 52) * s;
    };
    const auto layoutOf = [&](const MenuEntry& e, bool cell, ImVec2 at, float width) {
        const float line = lineOf(e, cell);
        return CardLayout(e, {at, ImVec2(at.x + width, at.y + line)}, CardShare(e, width, line, listWidth > 0, !cell && !home && e.height <= 0));
    };
    const auto heightOf = [&](const MenuEntry& e, bool cell, float width) {
        const MenuRect card = layoutOf(e, cell, ImVec2(0, 0), width).row;
        return card.y1 - card.y0;
    };
    const auto heads = [&](std::size_t i) { return !entries[i].group.empty() && (i == 0 || entries[i - 1].group != entries[i].group); };
    // A flyout whose cards do not fit its height (Training's pages at 150%)
    // shows them in two columns where they still have room, the first half
    // down the left: still one list in reading order, so Down from the
    // foot of the left column goes on to the top of the right.
    float stacked = 0;
    for (std::size_t i = 0; i < entries.size() && !grid; ++i) stacked += heightOf(entries[i], false, list.W()) + gap + (heads(i) ? 26 * s + gap : 0);
    const bool halves = f.flyout && !grid && stacked > list.H() && list.W() >= 520 * s;
    const std::size_t half = halves ? (entries.size() + 1) / 2 : entries.size();
    v2::Box focusCard{};
    ImGui::SetCursorScreenPos(list.min);
    if (changed) ImGui::SetNextWindowScroll(ImVec2(0, navigation.Scroll()));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    // Named as the render tests find it (ui_render_training.hxx, room_panel_navigation_test.cxx).
    ImGui::BeginChild("List", ImVec2(list.W(), list.H()), 0, ImGuiWindowFlags_NoNavInputs);
    auto* ld = ImGui::GetWindowDrawList();
    const float full = halves ? (ImGui::GetContentRegionAvail().x - gap) * .5f : ImGui::GetContentRegionAvail().x;
    const float cellWidth = grid ? (full - gap * (f.columns - 1)) / f.columns : full;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i == half && halves) ImGui::SetCursorScreenPos(ImVec2(top.x + full + gap, top.y));
        const auto& e = entries[i];
        // Home's "awards" row is the player's card in the pane.
        if (home && split && e.id == "awards") continue;
        const bool focused = e.id == navigation.Focus(), enabled = feedback_.Enabled(e), cell = i < cells;
        if (cell && i % f.columns) ImGui::SameLine(0, gap);
        // A group's heading over its first card.
        if (!cell && heads(i)) {
            if (i && i != half) ImGui::Dummy(ImVec2(0, 8 * s));
            const auto at = ImGui::GetCursorScreenPos();
            v2::SectionHeading(ld, ImVec2(at.x + 2 * s, at.y + 4 * s), e.group);
            ImGui::Dummy(ImVec2(full, 18 * s));
        }
        const float w = cell ? cellWidth : full;
        const MenuRowLayout layout = layoutOf(e, cell, ImGui::GetCursorScreenPos(), w);
        const v2::Box c = BoxOf(layout.row);
        ImGui::PushID(e.id.c_str());
        const bool pressed = ImGui::InvisibleButton("##card", ImVec2(c.W(), c.H()));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        ReportMenuCard(e.id.c_str(), c.min, c.max);
        if (pressed && listEnabled && !f.modalAtStart && !navigation.Editing() && !navigation.Asking()) {
            navigation.Focus(e.id, entries);
            // The value steps where it is drawn; anything else on the card is Select.
            const int step = StepAtPointer(e, layout);
            f.action = step ? MenuAction{MenuAction::Adjust, e.id, {}, step} : navigation.Choose(entries);
        } else if (listEnabled && pointerMoved && hovered && !navigation.Choosing()) navigation.Focus(e.id, entries);
        if (focused) {
            focusCard = c;
            if (lastFocus_ != e.id || changed) ImGui::SetScrollHereY(.5f);
        }
        if (!ImGui::IsItemVisible()) continue;
        v2::CardGround(ld, c, focused, hovered, e.valueHighlighted);
        // A screen's own drawing of its rows (portraits, flags, rankings).
        if (f.card && f.card(e, c.min, c.max)) { if (focused) v2::Focus(ld, c.min, c.max, !cell); continue; }
        DrawCardContent(ld, e, c, layout, enabled, twoLines && !cell, home, listWidth > 0);
        if (focused) v2::Focus(ld, c.min, c.max, !cell);
    }
    ImGui::PopStyleVar();
    navigation.Scroll() = ImGui::GetScrollY();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    return focusCard;
}

void GameMenu::DrawHomePane(Frame& f, v2::Box pane) {
    const float s = Scale();
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    // The player's card (the L tag: title, icon, facts) on top, the news under it.
    const float cardHeight = v2::Tag(d, pane.min, pane.W(), MenuPlayerCard().profile, v2::TagSize::L);
    NoteUserText(MenuPlayerCard().profile.title);
    // The card is the "awards" row: Select, or a click, opens the awards wall.
    if (const MenuEntry* row = FocusedIn(f.entries, "awards")) {
        const v2::Box card{pane.min, ImVec2(pane.max.x, pane.min.y + cardHeight)};
        ImGui::SetCursorScreenPos(card.min);
        const bool pressed = ImGui::InvisibleButton("##player-card", ImVec2(card.W(), card.H())), hovered = ImGui::IsItemHovered();
        if (pressed && listEnabled && !f.modalAtStart && !navigation.Editing() && !navigation.Asking()) { navigation.Focus(row->id, f.entries); f.action = navigation.Choose(f.entries); }
        else if (listEnabled && PointerMoved() && hovered && !navigation.Choosing()) navigation.Focus(row->id, f.entries);
        if (row->id == navigation.Focus()) v2::Focus(d, card.min, card.max);
    }
    v2::CutTop(pane, cardHeight, 16 * s);
    const auto& news = MenuNews();
    const float head = 40 * s, item = 56 * s;
    const std::size_t shown = (std::min)(news.size(), static_cast<std::size_t>((std::max)(0.f, (pane.H() - head - 36 * s) / item)));
    if (!shown) return;
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

void GameMenu::DrawDetailPane(Frame& f, v2::Box pane) {
    const float s = Scale();
    const MenuEntry& e = *FocusedIn(f.entries, navigation.Focus());
    ImGui::SetCursorScreenPos(pane.min);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, paneLeft ? ImVec4(0, 0, 0, 0) : ImGui::ColorConvertU32ToFloat4(v2::PanelGround()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 16 * s));
    // Beside Online play's cards the pane is as tall as what it says; a screen's own pane (News) takes the height.
    const bool fit = listWidth > 0 && !customDetail;
    if (fit) ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, pane.H()));
    ImGui::BeginChild("Detail", ImVec2(pane.W(), fit ? 0 : pane.H()),
        ImGuiChildFlags_AlwaysUseWindowPadding | (fit ? ImGuiChildFlags_AutoResizeY : 0), ImGuiWindowFlags_NoNavInputs);
    if (!customDetail) {
        if (e.userText) NoteUserText(e.label);
        v2::Paragraph(e.label, 22 * s, v2::Colors().text, HeadingFont());
        if (!e.detail.empty()) { NoteDetailText(e.detail, e.detailText); v2::Paragraph(e.detail, 16 * s, v2::Colors().muted); }
        if (!feedback_.Enabled(e) && !e.quiet) ImGui::TextDisabled("%s", loc::T(feedback_.Pending(e) ? "room.updating" : "common.unavailable"));
    }
    if (f.detail) f.detail(e.id);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

bool GameMenu::DrawValuePanel(Frame& f, v2::Box focusCard, v2::Box list) {
    const float s = Scale();
    const auto win = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    if (valuesClose_ && navigation.Choosing()) navigation.Cancel();
    valuesClose_ = false;
    const MenuEntry* focused = FocusedIn(f.entries, navigation.Focus());
    valuesAcross_ = 0;
    if (!navigation.Choosing() || !focused || focusCard.W() <= 0 || !notice_.empty()) { valuesOpen_ = false; return false; }
    const auto& e = *focused;
    // Select on a switch flips it: no popup for two words.
    if (MenuValueIsToggle(e)) { navigation.Cancel(); f.action = {MenuAction::Adjust, e.id, {}, e.value == loc::T("common.on") ? -1 : 1}; return false; }
    // As many across as fit inside the column, less the panel's padding and its edge's.
    const auto grid = MenuChoiceLayout(e.choices, list.W() - 24 * s);
    // The click that opened it is not a click outside it.
    const bool opening = !valuesOpen_;
    valuesOpen_ = true;
    const std::size_t count = e.choices.size(), across = e.popout ? 1 : grid.across, lines = (count + across - 1) / across;
    valuesAcross_ = across;
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
    ImGui::Begin("###Values", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
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
    const bool pointerMoved = PointerMoved();
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
        if (pressed && option.enabled) f.action = navigation.Pick(i, f.entries);
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
}

void GameMenu::DrawHintBar(Frame& f, v2::Box bar) {
    // What the buttons do now, each also a button that presses it; the status
    // on the right. An open dialog names its own Select and Back
    // (DialogLegend) and owns the input, so the screen's shortcuts, its tab
    // steps and the bell wait for it to close; a list that takes no input (a
    // capture) shows only the screen's own hints. A popup (a notice, the
    // editor, a confirmation, a choice drawn as one: Finish) takes the pointer
    // from the bar, so under one the hints are its buttons' names, as labels;
    // the reader and a board's value panel are no popups, and theirs are buttons.
    std::vector<v2::Hint> hints;
    std::vector<unsigned> presses;
    const auto hint = [&](const char* glyph, std::string label, unsigned press, bool labelOnly = false) {
        hints.push_back({glyph, std::move(label), labelOnly});
        presses.push_back(press);
    };
    const MenuEntry* focused = FocusedIn(f.entries, navigation.Focus());
    std::string dialogSelect, dialogBack;
    const bool dialog = DialogLegend(f.entries, dialogSelect, dialogBack);
    if (dialog) {
        const bool popup = !notice_.empty() || navigation.Editing() || navigation.Confirming() || (navigation.Choosing() && !f.body && !f.valuePanel);
        if (!dialogSelect.empty()) hint(MenuSelectGlyph(), dialogSelect, MenuInput::Select, popup);
        hint(MenuBackGlyph(), dialogBack, MenuInput::Back, popup);
    } else if (listEnabled) {
        MenuEntry shown;
        if (focused) { shown = *focused; shown.enabled = feedback_.Enabled(*focused); }
        if (const char* primary = MenuPrimaryHint(focused ? &shown : nullptr)) hint(MenuSelectGlyph(), primary, MenuInput::Select);
        if (!backInert) hint(MenuBackGlyph(), backHint.empty() ? loc::T("common.back") : backHint, MenuInput::Back);
        if (shown.adjustable && !MenuValueIsToggle(shown) && shown.enabled) hint(KeyboardPrompts() ? "keys-horizontal" : "horizontal", loc::T("menu.adjust"), MenuInput::Right);
    }
    if (!dialog) {
        for (const auto& h : shortcutHints) hint(h.glyph, h.label, h.press);
        // Tabs step with the device's tab buttons (LB and RB, Page Up and Down).
        if (!tabs.empty()) {
            const char* previous = MenuShortcutGlyph(MenuInput::PrevTab);
            const char* next = MenuShortcutGlyph(MenuInput::NextTab);
            if (previous) hint(previous, next ? "" : loc::T("menu.pages"), MenuInput::PrevTab);
            if (next) hint(next, loc::T("menu.pages"), MenuInput::NextTab);
        }
    }
    const bool hasStatus = f.status && *f.status, bell = !statusLeads.empty() && !dialog;
    const int hit = v2::HintBar(bar, hints, hasStatus ? f.status : footerNote,
        hasStatus ? ImGui::ColorConvertFloat4ToU32(ToneColor(f.statusTone)) : v2::Colors().muted, bell);
    // The bell opens its screen; a hint pressed is its button pressed, which
    // the next frame reads as it reads the pad (ReadFrame), so whatever owns
    // the input then (a choice, a reader, a toast, the list) answers it as it
    // answers the button.
    if (hit == static_cast<int>(presses.size())) f.action = {MenuAction::Shortcut, statusLeads};
    else if (hit >= 0) hintPress_ |= presses[hit];
}
} }
