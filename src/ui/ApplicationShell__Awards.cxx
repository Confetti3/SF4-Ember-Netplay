// The awards wall (Awards.dc.html), the last tab of Profile (ProfilePanel): the
// milestone awards (netplay/Awards.hxx), one tile for each family. A family has
// levels, and its icon and plate are in the colour of the highest one reached (Ash
// to Ember, how hard it is to reach); a pip for each level, and how far the next
// is. Select on an earned one offers Wear title, Wear icon or Wear both, which wear
// its current level; the player's card beside or above the wall, as others see it,
// shows the result. Each family is a row ("award:<family>"), so the pad, the keys
// and a screen reader walk the wall as a list. The levels come from the counters in
// the view alone: the points answer and the friends list.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "NameMarks.hxx"
#include "PlayerCard.hxx"
#include "ProfilePanel.hxx"
#include "SignInSites.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>

namespace sf4e { namespace ui {
namespace {
namespace aw = netplay::awards;
constexpr const char* AwardPrefix = "award:";

// The catalog id of the line above the wall while the points are not there to
// earn from, or null.
const char* StateWords(CardLevels levels) {
    switch (levels) {
    case CardLevels::Pending: return "level.loading";
    case CardLevels::NoAccount: return "awards.link_discord";
    case CardLevels::Unavailable: return "level.unavailable";
    case CardLevels::Unreachable: return "level.unreachable";
    case CardLevels::Counted: return nullptr;
    }
    return nullptr;
}

const aw::Family* FamilyOfRow(const MenuEntry& e) {
    return e.id.compare(0, 6, AwardPrefix) == 0 ? aw::Find(e.id.substr(6)) : nullptr;
}

std::string TierOf(const aw::Level& level) { return v2::TierName(v2::FrameTier(level.tier)); }
}

aw::Counters ApplicationShell::AwardCounters(const ShellView& v) {
    const auto* friends = v.friends.state.value.get();
    const std::uint32_t count = friends && friends->kind == netplay::friends::State::Kind::Listed ? static_cast<std::uint32_t>(friends->friends.size()) : 0;
    return aw::CountersOf(v.levels.value, count);
}
aw::Worn ApplicationShell::WornAward(const ShellView& v, const std::string& id) { return aw::WornOf(AwardCounters(v), id); }
std::string ApplicationShell::LevelTitle(const aw::Level& level) {
    return level.band ? loc::Tf("class.tip", ClassName(static_cast<std::uint8_t>(level.band))) : std::string(loc::T(level.title));
}
std::string ApplicationShell::LevelHow(const aw::Level& level) {
    return level.band ? loc::Tf("award.rank.how", ClassName(static_cast<std::uint8_t>(level.band))) : std::string(loc::T(level.how));
}
std::string ApplicationShell::AwardTitle(const aw::Worn& worn) { return LevelTitle(worn.Current()); }
std::string ApplicationShell::AwardIconName(const aw::Family& family) { return loc::T((std::string("award.") + family.id + ".icon").c_str()); }
std::string ApplicationShell::EarnedCount(const ShellView& v) {
    return loc::Tf("awards.earned_count", aw::LevelsEarned(AwardCounters(v)), static_cast<int>(aw::LevelTotal()));
}
int ApplicationShell::AwardColumns() { return v2::Narrow(ImGui::GetIO().DisplaySize.x, Scale()) ? 3 : 4; }

// A level a points answer reached that the one before did not is said once, with
// its tier. The first answer of a run only notes what is reached, so an update that
// adds awards does not announce them all, and an answer that is not counted says
// nothing. Levels reached together go in one toast.
void ApplicationShell::UpdateAwardNotices(const ShellView& v) {
    if (v.levels.revision == awardRevision_) return;
    awardRevision_ = v.levels.revision;
    const bool counted = v.levels.loaded && v.levels.value.kind == netplay::levels::Own::Kind::Counted;
    std::string titles;
    for (const auto& gained : awardWatch_.Update(AwardCounters(v), counted)) {
        const auto& level = *aw::LevelAt(*gained.family, gained.level);
        titles += (titles.empty() ? "" : ", ") + LevelTitle(level) + " \xC2\xB7 " + TierOf(level);
    }
    if (!titles.empty()) Say(loc::Tf("awards.notice", titles), Tone::Success, 5);
}

// A worn award the catalog no longer has is dropped from what is saved, and so
// is one the player's counted points no longer reach. A friends award is dropped
// only when it is gone: the friends list may simply not have arrived.
void ApplicationShell::DropStaleAwards(const ShellView& v) {
    const bool known = v.levels.loaded && v.levels.value.kind == netplay::levels::Own::Kind::Counted;
    const auto counters = AwardCounters(v);
    for (std::string* id : {&preferences_.wornTitle, &preferences_.wornIcon}) {
        if (id->empty()) continue;
        const auto* family = aw::Find(*id);
        if (!family || (known && family->counter != aw::Counter::Friends && !aw::Earned(counters, *family))) id->clear();
    }
}

// The focused family's words: each level with its tier, title and requirement,
// Earned on the ones reached and how far along the next one is.
std::string ApplicationShell::AwardLevelLines(const aw::Counters& counters, const aw::Family& family, bool counted) {
    const int level = aw::LevelOf(counters, family);
    const auto next = aw::Next(counters, family);
    std::string lines;
    for (std::size_t i = 0; i < family.count; ++i) {
        const auto& one = family.levels[i];
        std::string state;
        if (static_cast<int>(i) < level) state = loc::T("awards.earned");
        else if (static_cast<int>(i) == level && counted && aw::Counts(family)) state = loc::Tf("awards.progress", next.have, next.need);
        if (!lines.empty()) lines += "\n";
        lines += TierOf(one) + " \xC2\xB7 " + ApplicationShell::LevelTitle(one) + " \xC2\xB7 " + ApplicationShell::LevelHow(one) + (state.empty() ? "" : " \xC2\xB7 " + state);
    }
    return lines;
}

// Wear's options as a row offers them: each is off while that part of the
// card already wears the family.
std::vector<MenuEntry> ApplicationShell::AwardRows(const ShellView& v, std::string& title) const {
    title = loc::T("screen.awards");
    const auto counters = AwardCounters(v);
    PlayerCardView card;
    player_card::Fill(card, v.levels, preferences_.mainFighter);
    const bool counted = card.levels == CardLevels::Counted;
    std::vector<MenuEntry> rows;
    for (const auto& family : aw::Catalog) {
        const int level = aw::LevelOf(counters, family);
        const bool earned = level > 0, asTitle = earned && preferences_.wornTitle == family.id, asIcon = earned && preferences_.wornIcon == family.id, on = asTitle || asIcon;
        const auto next = aw::Next(counters, family);
        std::string detail = AwardLevelLines(counters, family, counted);
        if (on) detail = std::string(loc::T("awards.detail_worn")) + "\n" + detail;
        else if (earned) detail = std::string(loc::T("awards.detail_earned")) + "\n" + detail;
        auto row = Row(std::string(AwardPrefix) + family.id, earned ? LevelTitle(*aw::LevelAt(family, level)) : std::string(loc::T(family.name)), detail, earned && v.canEditPreferences);
        row.value = on ? std::string(loc::T("awards.on_card")) : next.top ? std::string(loc::T("awards.top_level")) : earned ? std::string(loc::T("awards.earned"))
            : std::string(loc::T("awards.locked"));
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
    // Without a Discord account there is nothing to earn from: the way to one, as the Card tab has it.
    if (card.levels == CardLevels::NoAccount) rows.push_back(ConnectDiscordRow());
    return rows;
}

// The wall, with the levels earned in its header.
class ApplicationShell::AwardsScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return screen == "awards"; }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override { return f.shell.AwardRows(f.view, title); }
    // An earned award's popout wears its title, its icon or both on the card.
    bool Choose(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        if (a.id.compare(0, 6, AwardPrefix) != 0) return false;
        auto& s = f.shell;
        if (!f.view.canEditPreferences) return true;
        const std::string id = a.id.substr(6);
        // Only a family the counters have a level of goes on the card.
        if (!WornAward(f.view, id)) return true;
        if (a.text != "icon") s.preferences_.wornTitle = id;
        if (a.text != "title") s.preferences_.wornIcon = id;
        s.preferencesDirty_ = true;
        s.saveAt_ = ImGui::GetTime() + .45;
        return true;
    }
    // Select on a locked award reads it: nothing to do. Connect Discord is the Ember ID panel's.
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        if (a.id == DiscordConnectScreen) { f.shell.identity_.Activate(a, f.view, f.shell.menu_.navigation); return true; }
        return a.id.compare(0, 6, AwardPrefix) == 0;
    }
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

// The card as others see it, then where else it shows (S, XS), on the left and kept
// in view while the wall scrolls, with the focused family's words under the card;
// the wall of tiles beside it, one for each family. Narrow, the card and the words
// go on top and the wall under them, and the body scrolls.
void ApplicationShell::DrawAwardsBoard(const ShellView& v, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float,
    const MenuVisualFeedback& feedback) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const auto& focus = nav.Focus();
    const bool changed = focus != awardsFocus_;
    const bool narrow = v2::Narrow(ImGui::GetIO().DisplaySize.x, s);
    awardsFocus_ = focus;
    const auto counters = AwardCounters(v);
    const auto& player = MenuPlayerCard();
    const auto origin = ImGui::GetCursorScreenPos();
    v2::Box area{origin, ImVec2(origin.x + width, FLT_MAX)};
    v2::Box side = narrow ? area : v2::CutLeft(area, (std::min)(440 * s, width * .4f), 24 * s);
    // Beside the wall the column stays where the wall's scroll has left the top of the body.
    if (!narrow) side.min.y = (std::max)(origin.y, ImGui::GetWindowPos().y);
    const float sideTop = side.min.y;
    // The card as others see it, and the smaller tags of rows and chat.
    const auto heading = [&](const char* text) {
        v2::SectionHeading(d, side.min, text);
        v2::CutTop(side, 18 * s, 10 * s);
    };
    const auto& profile = player.profile;
    heading(loc::T("awards.card_heading"));
    auto card = OwnAsOthers(player);
    card.art = v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait;
    DrawOtherCard(d, v2::CutTop(side, 160 * s, 16 * s), card);
    // The focused family's words: its name, then every level and where the player stands.
    const auto shown = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == focus && FamilyOfRow(e); });
    if (shown != rows.end()) {
        const v2::Box words{side.min, ImVec2(side.max.x, side.min.y + v2::WordsHeight(side.W(), shown->detail))};
        v2::Words(d, words, shown->label, shown->detail, true);
        v2::CutTop(side, words.H(), 16 * s);
    }
    if (!narrow) {
        heading(loc::T("awards.where_heading"));
        auto small = profile;
        small.facts.clear();
        v2::CutTop(side, v2::Tag(d, side.min, side.W(), small, v2::TagSize::S), 6 * s);
        v2::CutTop(side, v2::Tag(d, side.min, side.W(), small, v2::TagSize::XS), 6 * s);
    } else area.min.y = side.min.y + 8 * s;
    const float sideHeight = side.min.y - sideTop;

    // The state of the points, in words, and the way to Discord when there is no account.
    const float gap = 8 * s;
    float y = area.min.y;
    if (const char* words = StateWords(player.levels)) y += v2::Paragraph(d, ImVec2(area.min.x, y), area.W(), loc::T(words), 14 * s, c.muted) + 12 * s;
    const float before = y;
    y = DrawRowButton(DiscordConnectScreen, ImVec2(area.min.x, y), (std::min)(area.W(), 360 * s), rows, nav, action, changed);
    if (y > before) y += 12 * s;

    // The wall: a tile for each family, its icon on the colours of the level reached.
    const int columns = AwardColumns();
    const bool counted = player.levels == CardLevels::Counted;
    const float tileWidth = (area.W() - gap * (columns - 1)) / columns, tileHeight = 156 * s, icon = 52 * s, pip = 8 * s, pipGap = 3 * s;
    int column = 0;
    float bottom = y;
    for (const auto& e : rows) {
        const auto* family = FamilyOfRow(e);
        if (!family) continue;
        const ImVec2 at(area.min.x + column * (tileWidth + gap), y);
        const v2::Box tile{at, ImVec2(at.x + tileWidth, at.y + tileHeight)};
        bottom = tile.max.y;
        if (++column == columns) { column = 0; y += tileHeight + gap; }
        const bool hovered = PressEntry(nav, rows, e, tile, action, changed).hovered;
        const bool focused = e.id == focus, earned = !e.info, on = e.valueHighlighted;
        const int level = aw::LevelOf(counters, *family);
        const aw::Step next = aw::Next(counters, *family);
        v2::CardGround(d, tile, focused, hovered);
        if (on && !focused) d->AddRect(tile.min, tile.max, v2::color::Alpha(c.accent, .5f), 4 * s, 0, Hairline());
        // The name on two lines when it needs them; the icon, name, pips and progress centred as a stack.
        auto* font = HeadingFont();
        const float size = 14 * s, inner = tile.W() - 16 * s;
        const char *text = e.label.c_str(), *stop = text + e.label.size();
        const char* cut = font->CalcTextSizeA(size, FLT_MAX, 0, text, stop).x > inner ? font->CalcWordWrapPositionA(size / font->FontSize, text, stop, inner) : stop;
        const float lines = cut < stop ? 2.f : 1.f;
        const auto tier = v2::TierColor(earned ? v2::FrameTier(aw::LevelAt(*family, level)->tier) : v2::Tier::None);
        // How far the next level is: a thin meter over "37 of 100" for a count, the next level's name for a class or a frame, or the top.
        const bool meter = counted && !next.top && aw::Counts(*family);
        const float stack = icon + 6 * s + lines * (size + 2 * s) + 6 * s + pip + 6 * s + (meter ? 9 * s : 0) + 14 * s;
        const ImVec2 box(tile.min.x + (tile.W() - icon) * .5f, tile.min.y + (std::max)(6 * s, (tile.H() - stack) * .5f));
        d->AddRectFilled(box, ImVec2(box.x + icon, box.y + icon), earned ? v2::color::Alpha(tier.plate, .5f) : c.frame);
        d->AddRect(box, ImVec2(box.x + icon, box.y + icon), earned ? tier.stripe : c.border, 0, 0, 2 * s);
        v2::Icon(d, family->icon, ImVec2(box.x + 10 * s, box.y + 10 * s), icon - 20 * s, earned ? tier.stripe : v2::color::Alpha(c.muted, .5f));
        const auto line = [&](const char* from, const char* to, float top) {
            const float w = font->CalcTextSizeA(size, FLT_MAX, 0, from, to).x;
            d->AddText(font, size, ImVec2(tile.min.x + (tile.W() - w) * .5f, top), earned && feedback.Enabled(e) ? c.text : c.muted, from, to);
        };
        d->PushClipRect(tile.min, tile.max, true);
        line(text, cut, box.y + icon + 6 * s);
        while (cut < stop && *cut == ' ') ++cut;
        if (cut < stop) line(cut, stop, box.y + icon + 8 * s + size);
        // A pip for each level, in its own tier's colour up to the level reached.
        float py = box.y + icon + 10 * s + lines * (size + 2 * s);
        const float pips = family->count * pip + (family->count - 1) * pipGap;
        for (std::size_t i = 0; i < family->count; ++i) {
            const float px = tile.min.x + (tile.W() - pips) * .5f + i * (pip + pipGap);
            const bool reached = static_cast<int>(i) < level;
            const ImVec2 p0(px, py), p1(px + pip, py + pip);
            if (reached) d->AddRectFilled(p0, p1, v2::TierColor(v2::FrameTier(family->levels[i].tier)).stripe);
            else { d->AddRectFilled(p0, p1, c.frame); d->AddRect(p0, p1, c.border, 0, 0, Hairline()); }
        }
        py += pip + 6 * s;
        if (meter) {
            v2::Meter(d, {ImVec2(tile.min.x + 12 * s, py), ImVec2(tile.max.x - 12 * s, py + 5 * s)}, next.Part(), 0.f);
            py += 9 * s;
        }
        std::string state;
        if (next.top) state = loc::T("awards.top_level");
        else if (!counted) state = loc::T("awards.locked");
        else if (aw::Counts(*family)) state = loc::Tf("awards.progress", next.have, next.need);
        else state = loc::Tf("awards.next", ApplicationShell::LevelTitle(family->levels[level]));
        const float sw = ImGui::GetFont()->CalcTextSizeA(12 * s, FLT_MAX, 0, state.c_str()).x;
        d->AddText(ImGui::GetFont(), 12 * s, ImVec2(tile.min.x + (std::max)(4 * s, (tile.W() - sw) * .5f), py), on ? c.accent : c.muted, state.c_str());
        d->PopClipRect();
        if (focused) v2::Focus(d, tile.min, tile.max, false);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, (std::max)(bottom - origin.y, sideHeight) + gap));
}
} }
