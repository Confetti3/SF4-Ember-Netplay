// The awards wall (Awards.dc.html), the last tab of Profile (ProfilePanel): the
// eleven awards, earned or locked with how to earn it, the worn ones marked. Select
// on an earned one offers Wear title, Wear icon or Wear both; the player's card
// beside or above the wall, as others see it, shows the result. Each award is a
// row ("award:<id>"), so the pad, the keys and a screen reader walk the wall as a list.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "PlayerCard.hxx"
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

// Wear's options as a row offers them: each is off while that part of the
// card already wears the award.
std::vector<MenuEntry> ApplicationShell::AwardRows(const ShellView& v, std::string& title) const {
    title = loc::T("screen.awards");
    std::vector<MenuEntry> rows;
    for (const auto& a : v2::Awards) {
        const bool earned = Earned(v, a.id), asTitle = earned && preferences_.wornTitle == a.id, asIcon = earned && preferences_.wornIcon == a.id, on = asTitle || asIcon;
        const std::string tier = v2::TierName(a.tier);
        auto row = Row(std::string("award:") + a.id, loc::T(a.title),
            tier + " \xC2\xB7 " + (on ? std::string(loc::T("awards.detail_worn")) : earned ? std::string(loc::T("awards.detail_earned")) : loc::Tf("awards.detail_locked", loc::T(a.how))),
            earned && v.canEditPreferences);
        row.value = loc::T(on ? "awards.on_card" : earned ? "awards.earned" : "awards.locked");
        row.valueHighlighted = on;
        if (earned) {
            row.hint = loc::T("awards.wear");
            const char* worn = loc::T("awards.detail_worn");
            row.choices = {{"title", loc::T("awards.wear_title"), asTitle ? worn : "", !asTitle},
                {"icon", loc::T("awards.wear_icon"), asIcon ? worn : "", !asIcon},
                {"both", loc::T("awards.wear_both"), asTitle && asIcon ? worn : "", !(asTitle && asIcon)}};
        } else row.info = true;
        rows.push_back(std::move(row));
    }
    return rows;
}

// The wall, with the earned count in its header.
class ApplicationShell::AwardsScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return screen == "awards"; }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override { return f.shell.AwardRows(f.view, title); }
    // An earned award's popout wears its title, its icon or both on the card.
    bool Choose(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        if (a.id.compare(0, 6, "award:") != 0) return false;
        auto& s = f.shell;
        if (!f.view.canEditPreferences) return true;
        const std::string id = a.id.substr(6);
        if (a.text != "icon") s.preferences_.wornTitle = id;
        if (a.text != "title") s.preferences_.wornIcon = id;
        s.preferencesDirty_ = true;
        s.saveAt_ = ImGui::GetTime() + .45;
        return true;
    }
    // Select on a locked award reads it: nothing to do.
    bool Activate(const ScreenFrame&, const MenuAction& a, const Submit&) const override { return a.id.compare(0, 6, "award:") == 0; }
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        ProfilePanel::Dress(menu, f.screen, f.shell.preferences_.displayName);
        menu.headerChips.push_back({EarnedCount(f.view), v2::Colors().accentLight});
    }
    ScreenGrid Grid(const ScreenFrame&) const override { return {AwardColumns(), 100}; }
    GameMenu::Body Body(const ScreenFrame& f) const override {
        ApplicationShell* s = &f.shell; const ShellView* v = &f.view;
        return [s, v](const std::vector<MenuEntry>& e, MenuNavigation& n, MenuAction& a, float h, const MenuVisualFeedback& fb) { s->DrawAwardsBoard(*v, e, n, a, h, fb); };
    }
};
const ScreenPanel& ApplicationShell::AwardsOwner() {
    static const AwardsScreens awards{};
    return awards;
}

// The card as others see it, then where else it shows (S, XS), on the left, the wall of tiles
// beside it and the focused award's words under the wall. Narrow, the card
// goes on top and the wall under it, and the body scrolls.
void ApplicationShell::DrawAwardsBoard(const ShellView& v, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float,
    const MenuVisualFeedback& feedback) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const auto& focus = nav.Focus();
    const bool changed = focus != awardsFocus_;
    const bool narrow = v2::Narrow(ImGui::GetIO().DisplaySize.x, s);
    awardsFocus_ = focus;
    const auto origin = ImGui::GetCursorScreenPos();
    v2::Box area{origin, ImVec2(origin.x + width, FLT_MAX)};
    v2::Box side = narrow ? area : v2::CutLeft(area, (std::min)(440 * s, width * .4f), 24 * s);
    // The card as others see it, and the smaller tags of rows and chat.
    const auto heading = [&](const char* text) {
        v2::SectionHeading(d, side.min, text);
        v2::CutTop(side, 18 * s, 10 * s);
    };
    const auto& profile = MenuPlayerCard().profile;
    heading(loc::T("awards.card_heading"));
    auto card = OwnAsOthers(MenuPlayerCard());
    card.art = v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait;
    DrawOtherCard(d, v2::CutTop(side, 160 * s, 16 * s), card);
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
        const bool hovered = PressEntry(nav, rows, e, tile, action, changed).hovered;
        const bool focused = e.id == focus, earned = !e.info, on = e.valueHighlighted;
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
        const v2::Box words{ImVec2(area.min.x, bottom + 2 * s), ImVec2(area.max.x, bottom + 2 * s + v2::WordsHeight(area.W(), shown->detail))};
        v2::Words(d, words, shown->label, shown->detail, true);
        bottom = words.max.y;
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, (std::max)(bottom, side.min.y) - origin.y));
}
} }
