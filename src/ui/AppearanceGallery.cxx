#include "AppearanceGallery.hxx"
#include "AppearanceGrid.hxx"
#include "FighterSelector.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "RoomCards.hxx"
#include "RowId.hxx"
#include "SelectionArt.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <sstream>

namespace sf4e { namespace ui {
namespace {
constexpr const char* CostumesScreen = "costumes";
constexpr const char* ColorsScreen = "colors";
constexpr const char* UltrasScreen = "ultras";
// An Ultra's picture, or the fighter in what they wear where it has none.
void DrawUltraPicture(const FighterPicture& figure, int ultra, ImVec2 min, ImVec2 max) {
    auto* art = MenuArt();
    const SelectionImage picture = art ? art->Ultra(figure.fighter, ultra) : SelectionImage{};
    if (art && !picture.missing) ImageInRect(picture, min, max);
    else DrawCharacterPortrait(figure, min, max, IM_COL32(0, 0, 0, 0));
}

// An Ultra's command as symbols (UltraCommand::symbols, facing right): a
// motion is an arrow, a curved one an arc on a dark disc with an Ember dot
// where it starts; a button is its letters in an Ember ring, three rings for
// PPP or KKK; a charge carries the word for it under its arrow. Vector paths,
// so they keep their shape at every size. `unit` is a symbol's side.
std::vector<std::string> SymbolTokens(const std::string& symbols) {
    std::istringstream stream(symbols);
    std::vector<std::string> tokens;
    for (std::string token; stream >> token;) tokens.push_back(token);
    return tokens;
}
float TokenUnits(std::string token) {
    if (!token.empty() && token[0] == '~') token.erase(0, 1);
    return token == "PPP" || token == "KKK" ? 2.4f : token == "+" ? .6f : 1.f;
}
constexpr float SymbolGap = .1f;
// The symbols' width, in units.
float SymbolUnits(const std::string& symbols) {
    float units = 0;
    for (const auto& token : SymbolTokens(symbols)) units += TokenUnits(token) + (units > 0 ? SymbolGap : 0);
    return units;
}
// A row of symbols: a unit tall, and the charge word's line under it.
float SymbolRowHeight(float unit) { return unit * 1.42f; }
void DrawSymbols(ImDrawList* d, ImVec2 origin, float unit, const std::string& symbols, const char* charge) {
    const float k = unit / 30;
    auto* font = ImGui::GetFont();
    const ImU32 ivory = palette::Ivory, ember = palette::Ember;
    const auto arrowHead = [&](ImVec2 tip, ImVec2 direction) {
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
        if (length < .001f) return;
        direction.x /= length; direction.y /= length;
        d->AddTriangleFilled(tip,
            ImVec2(tip.x - 7 * k * direction.x + 4 * k * direction.y, tip.y - 7 * k * direction.y - 4 * k * direction.x),
            ImVec2(tip.x - 7 * k * direction.x - 4 * k * direction.y, tip.y - 7 * k * direction.y + 4 * k * direction.x), ivory);
    };
    float x = 0;
    for (auto token : SymbolTokens(symbols)) {
        const bool held = token[0] == '~';
        if (held) token.erase(0, 1);
        const bool triple = token == "PPP" || token == "KKK";
        const bool button = token.find('P') != std::string::npos || token.find('K') != std::string::npos;
        const float width = TokenUnits(token) * unit;
        const ImVec2 p(origin.x + x, origin.y), center(p.x + unit / 2, p.y + unit / 2);
        if (button) {
            // LP, LK or HP keep both letters; PPP and KKK are three of one.
            const std::string label = triple ? token.substr(0, 1) : token;
            const float size = unit * (label.size() > 1 ? .4f : .52f);
            const auto measured = font->CalcTextSizeA(size, FLT_MAX, 0, label.c_str());
            for (int i = 0; i < (triple ? 3 : 1); ++i) {
                const ImVec2 c(center.x + i * unit * .7f, center.y);
                d->AddCircleFilled(c, unit * .44f, IM_COL32(48, 36, 25, 255));
                d->AddCircle(c, unit * .44f, ember, 24, 1.5f * k);
                d->AddText(font, size, ImVec2(c.x - measured.x / 2, c.y - measured.y / 2), ivory, label.c_str());
            }
        } else if (token == "+") {
            const float size = unit * .55f;
            const auto measured = font->CalcTextSizeA(size, FLT_MAX, 0, "+");
            d->AddText(font, size, ImVec2(p.x + (width - measured.x) / 2, center.y - measured.y / 2), palette::Muted, "+");
        } else if (token.size() == 1 && token[0] >= '1' && token[0] <= '9' && token[0] != '5') {
            // A direction on the numeric keypad: 6 forward, 2 down, 9 up-forward.
            const int digit = token[0] - '0';
            const ImVec2 direction(static_cast<float>((digit - 1) % 3 - 1), static_cast<float>(1 - (digit - 1) / 3));
            const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
            const ImVec2 unitDirection(direction.x / length, direction.y / length);
            const ImVec2 tip(center.x + 11 * k * unitDirection.x, center.y + 11 * k * unitDirection.y);
            d->AddLine(ImVec2(center.x - 10 * k * unitDirection.x, center.y - 10 * k * unitDirection.y), tip, ivory, 3 * k);
            arrowHead(tip, unitDirection);
        } else {
            // A quarter circle (236, 214), a half circle (63214) or a full one
            // (360), on a faint ring so the circle reads at a glance. The arc
            // turns the way the stick is pressed: a quarter circle forward
            // starts down and ends forward. The arrowhead sits on the arc's
            // end along its tangent, and an Ember dot marks where it starts.
            const float pi = 3.14159265f;
            d->AddCircleFilled(center, unit * .48f, IM_COL32(47, 58, 59, 255));
            const bool forward = token == "236", back = token == "214", half = token == "63214";
            const float radius = 11 * k;
            if (forward || back || half) d->AddCircle(center, radius, IM_COL32(240, 232, 214, 56), 32, 1.4f * k);
            const float begin = forward ? pi : back ? 0.f : half ? -pi / 2 : pi / 2;
            const float end = forward || back ? pi / 2 : half ? pi / 2 : pi / 2 - 1.8f * pi;
            const float turn = end > begin ? 1.f : -1.f, head = 5 * k;
            // The line stops where the arrowhead begins.
            const float stop = end - turn * head / radius;
            ImVec2 points[33];
            for (int i = 0; i < 33; ++i) {
                const float angle = begin + (stop - begin) * i / 32;
                points[i] = ImVec2(center.x + radius * std::cos(angle), center.y + radius * std::sin(angle));
            }
            d->AddPolyline(points, 33, ivory, 0, 3 * k);
            const ImVec2 tip(center.x + radius * std::cos(end), center.y + radius * std::sin(end));
            const ImVec2 along(-std::sin(end) * turn, std::cos(end) * turn);
            const ImVec2 base(tip.x - head * along.x, tip.y - head * along.y);
            d->AddTriangleFilled(tip, ImVec2(base.x - 3.6f * k * along.y, base.y + 3.6f * k * along.x),
                ImVec2(base.x + 3.6f * k * along.y, base.y - 3.6f * k * along.x), ivory);
            d->AddCircleFilled(points[0], 2.4f * k, ember);
        }
        if (held) {
            // The word for a charge under its arrow, shrunk to the arrow's width.
            const float natural = unit * .36f, measured = font->CalcTextSizeA(natural, FLT_MAX, 0, charge).x;
            const float size = (std::min)(natural, natural * width / (std::max)(1.f, measured));
            const float w = font->CalcTextSizeA(size, FLT_MAX, 0, charge).x;
            d->AddText(font, size, ImVec2(p.x + (width - w) / 2, p.y + unit + 1 * k), ember, charge);
        }
        x += width + SymbolGap * unit;
    }
}
// `text` on one line `width` wide: shrunk to three quarters of `size` where
// that makes it fit, cut short past that.
void FittedText(ImVec2 at, float width, const std::string& text, float size, ImU32 color) {
    const float measured = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x;
    if (measured > width) size = (std::max)(size * .75f, size * width / measured);
    std::string elided;
    DrawCardText(elided, at, width, text, size, color);
}
}
bool AppearanceGallery::Shows(const std::string& screen) { return screen == CostumesScreen || screen == ColorsScreen || screen == UltrasScreen; }
const char* AppearanceGallery::Title(const std::string& screen) {
    return loc::T(screen == CostumesScreen ? "selection.costume_gallery_title" : screen == ColorsScreen ? "selection.color_gallery_title" :
        "selection.ultra_combo_title");
}
// A costume opens its colours; a colour or an Ultra saves.
const char* AppearanceGallery::StatusKey(const std::string& screen) {
    return screen == CostumesScreen ? "selection.status_browse" : "selection.status_editable";
}
void AppearanceGallery::Open(MenuNavigation& nav, Page page, const selection::Pick& pick) {
    costume_ = pick.costume;
    switch (page) {
    case Page::Costumes: nav.Push(CostumesScreen); nav.Prefer("costume-" + std::to_string(pick.costume)); break;
    case Page::Colors: nav.Push(ColorsScreen); nav.Prefer("color-" + std::to_string(pick.color)); break;
    case Page::Ultras: nav.Push(UltrasScreen); nav.Prefer("ultra-" + std::to_string(pick.ultra)); break;
    }
}
void AppearanceGallery::Leave(MenuNavigation& nav) {
    while (Shows(nav.Screen()) && nav.Screen() != nav.Root()) nav.Return();
}
std::vector<MenuEntry> AppearanceGallery::Build(const std::string& screen, const selection::Pick& pick, const selection::Availability& availability,
                                                const selection::InstalledContent* installed, bool editable) {
    using namespace selection;
    cards_.clear();
    page_ = screen == CostumesScreen ? Page::Costumes : screen == ColorsScreen ? Page::Colors : Page::Ultras;
    std::vector<MenuEntry> rows;
    const auto card = [&](std::string id, std::string label, std::string detail, bool enabled, int key, Card drawn) {
        auto row = Row(std::move(id), std::move(label), std::move(detail), enabled);
        row.key = static_cast<std::uint64_t>(key);
        rows.push_back(std::move(row));
        cards_.push_back(std::move(drawn));
    };
    // The Ultras are the edition's, whatever the player owns.
    if (page_ == Page::Ultras) {
        const FighterPicture worn{pick.fighter, FighterArt::Character, pick.costume, pick.color};
        for (const int ultra : AllowedUltras(pick.fighter, pick.edition)) {
            // Ultra Double's input is Ultra I's and Ultra II's: on the card each
            // under its name, in the detail each line after U1 or U2.
            std::string detail;
            Card drawn{worn, {}, ultra == pick.ultra};
            for (const auto& command : UltraInputs(pick.fighter, ultra, pick.edition)) {
                const std::string label = ultra == 2 ? ShortUltraLabel(command.ultra) : "";
                if (drawn.inputs.empty() || drawn.inputs.back().ultra != command.ultra)
                    drawn.inputs.push_back({command.ultra, label.empty() ? label : label + "  " + UltraMove(pick.fighter, command.ultra), {}});
                drawn.inputs.back().commands.push_back({command.symbols, command.words});
                detail += (detail.empty() ? "" : "\n") + (label.empty() ? command.words : label + " " + command.words);
            }
            card("ultra-" + std::to_string(ultra), UltraName(pick.fighter, ultra), std::move(detail), editable, ultra, std::move(drawn));
            if (editable) rows.back().hint = loc::T("menu.hint.save_ultra");
        }
        return rows;
    }
    // A room can open the grid before the game has said what the player owns.
    if (!availability.ready) return {Row("waiting", loc::T("selection.loading_choices"), loc::T("selection.loading_choices.detail"), false)};
    const char* locked = loc::T("selection.locked_detail");
    if (page_ == Page::Costumes) {
        for (const int costume : AllowedCostumes(pick.fighter, availability, installed)) {
            auto option = pick; option.costume = costume;
            const auto colors = AllowedColors(pick.fighter, costume, availability, installed);
            card("costume-" + std::to_string(costume), ShortCostumeLabel(option, installed),
                 colors.empty() ? loc::T("selection.no_colors") : editable ? CostumeLabel(option, installed) : std::string(locked),
                 editable && !colors.empty(), costume,
                 {{pick.fighter, FighterArt::Character, costume, GalleryColor(colors, pick.color)}, {}, costume == pick.costume});
        }
        return rows;
    }
    auto shown = pick; shown.costume = costume_;
    const std::string costumeName = CostumeLabel(shown, installed);
    for (const int color : AllowedColors(pick.fighter, costume_, availability, installed)) {
        // A custom colour is named by the setup program, else numbered like the rest.
        const std::string name = installed && IsCustomColor(color) ? installed->Name(pick.fighter, costume_, color) : std::string();
        card("color-" + std::to_string(color), name.empty() ? loc::Tf("selection.color", color + 1) : name,
             editable ? costumeName : std::string(locked), editable, color,
             {{pick.fighter, FighterArt::Character, costume_, color}, {}, costume_ == pick.costume && color == pick.color});
        if (editable) rows.back().hint = loc::T("menu.hint.save_color");
    }
    if (rows.empty()) rows.push_back(Row("no-colors", loc::T("selection.no_colors"), costumeName, false));
    return rows;
}
void AppearanceGallery::DrawBody(const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height) {
    if (cards_.empty() || cards_.size() != rows.size()) {
        // No cards: the row that says why.
        for (const auto& e : rows) { ImGui::TextWrapped("%s", e.label.c_str()); ImGui::TextWrapped("%s", e.detail.c_str()); }
        return;
    }
    const bool changed = nav.Focus() != focus_;
    focus_ = nav.Focus();
    if (page_ == Page::Ultras) { DrawUltras(rows, nav, action, height, changed); return; }
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 8 * s;
    const float font = (std::min)(ImGui::GetFontSize(), 15 * s);
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const float caption = font + 8 * s;
    const auto grid = FitGallery(rows.size(), width, height, gap, FigureCard.aspect, caption, FigureCard.narrowest * s, FigureCard.widest * s);
    const auto origin = ImGui::GetCursorScreenPos();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& e = rows[i];
        const auto& shown = cards_[i];
        const auto at = grid.Cell(i, width, gap);
        const v2::Box b{ImVec2(origin.x + at.x, origin.y + at.y), ImVec2(origin.x + at.x + grid.width, origin.y + at.y + grid.height)};
        const bool hovered = PressEntry(nav, rows, e, b, action, changed).hovered, focused = e.id == nav.Focus();
        v2::CardGround(d, b, focused, hovered, shown.worn);
        DrawCharacterPortrait(shown.figure, ImVec2(b.min.x + 3 * s, b.min.y + 3 * s), ImVec2(b.max.x - 3 * s, b.max.y - caption), IM_COL32(0, 0, 0, 0));
        if (!e.enabled) d->AddRectFilled(b.min, b.max, v2::color::Alpha(c.ink, .65f), 4 * s);
        d->AddRectFilled(ImVec2(b.min.x, b.max.y - caption), b.max, v2::color::Alpha(c.ink, .85f));
        std::string elided;
        DrawCardText(elided, ImVec2(b.min.x + 4 * s, b.max.y - caption + 4 * s), grid.width - 8 * s, e.label, font, shown.worn ? c.accent : c.text, true);
        if (focused) v2::Focus(d, b.min, b.max, false);
    }
    // The grid's whole height, so a grid taller than the body scrolls.
    const auto lines = static_cast<float>(grid.Lines(rows.size()));
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, lines * (grid.height + gap) - gap));
}
// The Ultra page as the old one drew it, at the size of the screen: each card
// its photo (Ultra Double a large W) with the game's mark top-right and SAVED
// top-left, its name, and each command's symbols over its line in words.
void AppearanceGallery::DrawUltras(const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height, bool changed) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 14 * s, pad = 12 * s;
    const float titleFont = (std::min)(ImGui::GetFontSize() * 1.1f, 20 * s), font = (std::min)(ImGui::GetFontSize(), 15 * s);
    const float titleLine = titleFont + 10 * s, line = font + 6 * s, between = 8 * s;
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    // Every card's caption is as tall as the one with the most to show, and
    // every symbol row as large as the longest command lets them be.
    float widest = 1;
    for (const auto& card : cards_) for (const auto& input : card.inputs) for (const auto& command : input.commands)
        widest = (std::max)(widest, SymbolUnits(command.symbols));
    const auto caption = [&](float unit) {
        float most = 0;
        for (const auto& card : cards_) {
            float h = 0;
            for (const auto& input : card.inputs)
                h += (input.heading.empty() ? 0 : line) + static_cast<float>(input.commands.size()) * (SymbolRowHeight(unit) + line) + between;
            most = (std::max)(most, h);
        }
        return pad + titleLine + most + pad - between;
    };
    const auto fit = [&](float unit) {
        return FitGallery(rows.size(), width, height, gap, UltraCard.aspect, caption(unit), UltraCard.narrowest * s, UltraCard.widest * s);
    };
    // The largest symbols are 44 px a side (at 1080p); narrower cards shrink them to fit.
    float unit = 44 * s;
    auto grid = fit(unit);
    const float fits = (grid.width - 2 * pad) / widest;
    if (fits < unit) { unit = fits; grid = fit(unit); }
    // A row of cards shorter than the page stands in its middle.
    const float lines = static_cast<float>(grid.Lines(rows.size())), whole = lines * (grid.height + gap) - gap;
    const auto origin = ImGui::GetCursorScreenPos();
    const float drop = (std::max)(0.f, (height - whole) * .5f);
    const char* charge = loc::T("selection.ultra_charge");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& e = rows[i];
        const auto& shown = cards_[i];
        const int ultra = static_cast<int>(e.key);
        const auto at = grid.Cell(i, width, gap);
        const v2::Box b{ImVec2(origin.x + at.x, origin.y + drop + at.y), ImVec2(origin.x + at.x + grid.width, origin.y + drop + at.y + grid.height)};
        const bool hovered = PressEntry(nav, rows, e, b, action, changed).hovered, focused = e.id == nav.Focus();
        v2::CardGround(d, b, focused, hovered, shown.worn);
        const v2::Box picture{ImVec2(b.min.x + 3 * s, b.min.y + 3 * s), ImVec2(b.max.x - 3 * s, b.min.y + 3 * s + (grid.width - 6 * s) * UltraCard.aspect)};
        if (ultra != 2) DrawUltraPicture(shown.figure, ultra, picture.min, picture.max);
        else {
            // Ultra Double: one large W on a dark panel, drawn as a shape so it
            // stays sharp at any size.
            d->AddRectFilled(picture.min, picture.max, IM_COL32(26, 22, 19, 255));
            const float tall = (std::min)(picture.H() * .6f, picture.W() * .56f), wide = tall * 1.25f;
            const ImVec2 corner((picture.min.x + picture.max.x - wide) * .5f, (picture.min.y + picture.max.y - tall) * .5f);
            static const float outline[][2] = {{0, 0}, {.2f, 0}, {.3f, .62f}, {.42f, 0}, {.58f, 0}, {.7f, .62f}, {.8f, 0}, {1, 0},
                {.82f, 1}, {.62f, 1}, {.5f, .38f}, {.38f, 1}, {.18f, 1}};
            ImVec2 points[13];
            for (int p = 0; p < 13; ++p) points[p] = ImVec2(corner.x + outline[p][0] * wide, corner.y + outline[p][1] * tall);
            d->AddConcavePolyFilled(points, 13, palette::Ember);
        }
        // The game's I, II or W top-right; SAVED top-left on the Ultra saved now.
        const float corner = 6 * s;
        DrawUltraMark(d, ImVec2(picture.max.x - corner, picture.min.y + corner), (std::min)(64 * s, (std::max)(20 * s, picture.H() * .2f)), ultra);
        if (shown.worn) DrawCardBadge(ImVec2(picture.min.x + corner, picture.min.y + corner), picture.W() * .5f, loc::T("selection.saved"), "saved-badge");
        d->AddRectFilled(ImVec2(b.min.x + 1, picture.max.y), ImVec2(b.max.x - 1, b.max.y - 1), v2::color::Alpha(c.ink, .85f), 4 * s, ImDrawFlags_RoundCornersBottom);
        const float x = b.min.x + pad, room = grid.width - 2 * pad;
        float y = picture.max.y + pad;
        FittedText(ImVec2(x, y), room, e.label, titleFont, shown.worn ? c.accent : c.text);
        y += titleLine;
        for (const auto& input : shown.inputs) {
            if (!input.heading.empty()) { FittedText(ImVec2(x, y), room, input.heading, font, c.text); y += line; }
            for (const auto& command : input.commands) {
                DrawSymbols(d, ImVec2(x, y), unit, command.symbols, charge);
                y += SymbolRowHeight(unit);
                // The same input in words that face neither way, for a player on the right.
                FittedText(ImVec2(x, y), room, command.line, font, c.muted);
                y += line;
            }
            y += between;
        }
        if (!e.enabled) d->AddRectFilled(b.min, b.max, v2::color::Alpha(c.ink, .65f), 4 * s);
        if (focused) v2::Focus(d, b.min, b.max, false);
    }
    // The cards' whole height, so cards taller than the body scroll.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, drop + whole));
}
AppearanceGallery::Choice AppearanceGallery::Choose(const std::string& id, MenuNavigation& nav, selection::Pick& pick,
                                                    const selection::Availability& availability, const selection::InstalledContent* installed) {
    if (id == "costume") { Open(nav, Page::Costumes, pick); return Choice::Opened; }
    if (id == "color") { Open(nav, Page::Colors, pick); return Choice::Opened; }
    if (id == "ultra") { Open(nav, Page::Ultras, pick); return Choice::Opened; }
    if (const auto costume = RowKey<int>(id, "costume-")) {
        costume_ = *costume;
        nav.Push(ColorsScreen);
        nav.Prefer("color-" + std::to_string(GalleryColor(selection::AllowedColors(pick.fighter, costume_, availability, installed), pick.color)));
        return Choice::Opened;
    }
    if (const auto color = RowKey<int>(id, "color-")) {
        pick.costume = costume_; pick.color = *color;
        Leave(nav);
        return Choice::Saved;
    }
    if (const auto ultra = RowKey<int>(id, "ultra-")) {
        pick.ultra = *ultra;
        Leave(nav);
        return Choice::Saved;
    }
    return Choice::Unhandled;
}
} }
