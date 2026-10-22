// The awards wall (Awards.dc.html): every title (on the Titles tab) and icon
// (on Icons), earned or locked with how to earn it, the one worn marked. Select
// on an earned one wears it; the player's card above or beside the wall shows
// the result. Each award is a row ("award:<id>"), so the pad, the keys and a
// screen reader walk the wall as a list; Classic shows that list as it is.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>

namespace sf4e { namespace ui {
namespace {
bool Earned(const ShellView& v, const std::string& id) { return std::find(v.earnedAwards.begin(), v.earnedAwards.end(), id) != v.earnedAwards.end(); }
}
const v2::Award* ApplicationShell::WornAward(const ShellView& v, const std::string& id) { return Earned(v, id) ? v2::FindAward(id) : nullptr; }
std::string ApplicationShell::EarnedCount(const ShellView& v) {
    int earned = 0;
    for (const auto& a : v2::Awards) earned += Earned(v, a.id);
    return loc::Tf("awards.earned_count", earned, static_cast<int>(std::size(v2::Awards)));
}
int ApplicationShell::AwardColumns() { return v2::Narrow(ImGui::GetIO().DisplaySize.x, Scale()) ? 3 : 4; }

std::vector<MenuEntry> ApplicationShell::AwardRows(const ShellView& v, const std::string& screen, std::string& title) const {
    title = loc::T("screen.awards");
    const bool titles = screen == "awards";
    const std::string& worn = titles ? preferences_.wornTitle : preferences_.wornIcon;
    std::vector<MenuEntry> rows;
    for (const auto& a : v2::Awards) {
        const bool earned = Earned(v, a.id), on = earned && worn == a.id;
        const std::string tier = v2::TierName(a.tier);
        auto row = Row(std::string("award:") + a.id, loc::T(titles ? a.title : a.name),
            tier + " \xC2\xB7 " + (on ? std::string(loc::T("awards.detail_worn")) : earned ? std::string(loc::T("awards.detail_earned")) : loc::Tf("awards.detail_locked", loc::T(a.how))),
            earned && v.canEditPreferences);
        row.value = loc::T(on ? "awards.on_card" : earned ? "awards.earned" : "awards.locked");
        row.valueHighlighted = on;
        if (earned) row.hint = loc::T("awards.wear"); else row.info = true;
        rows.push_back(std::move(row));
    }
    return rows;
}

// The card (L) with where else it shows (S, XS) on the left, the wall of tiles
// beside it and the focused award's words under the wall. Narrow, the card
// goes on top and the wall under it, and the body scrolls.
void ApplicationShell::DrawAwardsBoard(const ShellView& v, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float,
    const MenuVisualFeedback& feedback) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const auto& focus = nav.Focus();
    const auto mouse = ImGui::GetIO().MouseDelta;
    const bool pointerMoved = mouse.x != 0 || mouse.y != 0, changed = focus != awardsFocus_;
    const bool narrow = v2::Narrow(ImGui::GetIO().DisplaySize.x, s);
    awardsFocus_ = focus;
    const auto origin = ImGui::GetCursorScreenPos();
    v2::Box area{origin, ImVec2(origin.x + width, FLT_MAX)};
    v2::Box side = narrow ? area : v2::CutLeft(area, (std::min)(440 * s, width * .4f), 24 * s);
    // The card as others see it, and the smaller tags of rows and chat.
    const auto heading = [&](const char* text) {
        d->AddText(HeadingFont(), 14 * s, side.min, c.muted, text);
        v2::CutTop(side, 18 * s, 10 * s);
    };
    const auto& profile = MenuPlayerCard().profile;
    heading(loc::T("awards.card_heading"));
    v2::CutTop(side, v2::Tag(d, side.min, side.W(), profile, v2::TagSize::L), 16 * s);
    if (!narrow) {
        heading(loc::T("awards.where_heading"));
        auto small = profile;
        small.facts.clear();
        v2::CutTop(side, v2::Tag(d, side.min, side.W(), small, v2::TagSize::S), 6 * s);
        v2::CutTop(side, v2::Tag(d, side.min, side.W(), small, v2::TagSize::XS), 6 * s);
    } else area.min.y = side.min.y + 8 * s;
    // The wall: a tile per award, its icon on its tier's colours when earned.
    const int columns = AwardColumns();
    const float gap = 8 * s, tileWidth = (area.W() - gap * (columns - 1)) / columns, tileHeight = 128 * s, icon = 56 * s;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& e = rows[i];
        const auto* award = v2::FindAward(e.id.substr(6));
        if (!award) continue;
        const ImVec2 at(area.min.x + (i % columns) * (tileWidth + gap), area.min.y + (i / columns) * (tileHeight + gap));
        const v2::Box tile{at, ImVec2(at.x + tileWidth, at.y + tileHeight)};
        ImGui::SetCursorScreenPos(tile.min);
        ImGui::PushID(e.id.c_str());
        const bool pressed = ImGui::InvisibleButton("##award", ImVec2(tile.W(), tile.H())), hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (pressed && !nav.Asking() && !nav.Editing()) { nav.Focus(e.id, rows); action = nav.Choose(rows); }
        else if (hovered && pointerMoved) nav.Focus(e.id, rows);
        const bool focused = e.id == focus, earned = !e.info, on = e.valueHighlighted;
        if (focused && changed) ImGui::SetScrollHereY(.5f);
        v2::CardGround(d, tile, focused, hovered);
        if (on && !focused) d->AddRect(tile.min, tile.max, v2::color::Alpha(c.accent, .5f), 4 * s, 0, s);
        // The name on two lines when it needs them; the icon, name and state centred as a stack.
        auto* font = HeadingFont();
        const float size = 14 * s, inner = tile.W() - 16 * s;
        const char *text = e.label.c_str(), *stop = text + e.label.size();
        const char* cut = font->CalcTextSizeA(size, FLT_MAX, 0, text, stop).x > inner ? font->CalcWordWrapPositionA(size / font->FontSize, text, stop, inner) : stop;
        const float lines = cut < stop ? 2.f : 1.f;
        const auto tier = v2::TierColor(award->tier);
        const ImVec2 box(tile.min.x + (tile.W() - icon) * .5f, tile.min.y + (tile.H() - icon - lines * (size + 2 * s) - 24 * s) * .5f);
        d->AddRectFilled(box, ImVec2(box.x + icon, box.y + icon), earned ? v2::color::Alpha(tier.plate, .5f) : c.frame);
        d->AddRect(box, ImVec2(box.x + icon, box.y + icon), earned ? tier.stripe : c.border, 0, 0, 2 * s);
        v2::Icon(d, award->icon, ImVec2(box.x + 11 * s, box.y + 11 * s), icon - 22 * s, earned ? tier.stripe : v2::color::Alpha(c.muted, .5f));
        const auto line = [&](const char* from, const char* to, float y) {
            const float w = font->CalcTextSizeA(size, FLT_MAX, 0, from, to).x;
            d->AddText(font, size, ImVec2(tile.min.x + (tile.W() - w) * .5f, y), earned && feedback.Enabled(e) ? c.text : c.muted, from, to);
        };
        d->PushClipRect(tile.min, tile.max, true);
        line(text, cut, box.y + icon + 6 * s);
        while (cut < stop && *cut == ' ') ++cut;
        if (cut < stop) line(cut, stop, box.y + icon + 8 * s + size);
        const std::string state = on || !earned ? e.value : std::string(v2::TierName(award->tier)) + " \xC2\xB7 " + e.value;
        const float sw = ImGui::GetFont()->CalcTextSizeA(12 * s, FLT_MAX, 0, state.c_str()).x;
        d->AddText(ImGui::GetFont(), 12 * s, ImVec2(tile.min.x + (tile.W() - sw) * .5f, box.y + icon + 10 * s + lines * (size + 2 * s)), on ? c.accent : c.muted, state.c_str());
        d->PopClipRect();
        if (focused) v2::Focus(d, tile.min, tile.max, false);
    }
    // The focused award's words: its name and tier, then earned, worn or how to earn it.
    float bottom = area.min.y + ((rows.size() + columns - 1) / columns) * (tileHeight + gap);
    const auto shown = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == focus; });
    if (shown != rows.end()) {
        const float wrap = area.W() - 28 * s, size = 16 * s;
        const float h = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, wrap, shown->detail.c_str()).y + size + 26 * s;
        const v2::Box words{ImVec2(area.min.x, bottom + 2 * s), ImVec2(area.max.x, bottom + 2 * s + h)};
        d->AddRectFilled(words.min, words.max, c.frame, 4 * s);
        d->AddRect(words.min, words.max, c.border, 4 * s, 0, s);
        d->AddText(HeadingFont(), size, ImVec2(words.min.x + 14 * s, words.min.y + 10 * s), c.text, shown->label.c_str());
        d->AddText(ImGui::GetFont(), size, ImVec2(words.min.x + 14 * s, words.min.y + 14 * s + size), c.muted, shown->detail.c_str(), nullptr, wrap);
        bottom = words.max.y;
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, (std::max)(bottom, side.min.y) - origin.y));
}
} }
