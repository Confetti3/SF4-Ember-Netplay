#include "PlayerPage.hxx"
#include "AwardsWall.hxx"
#include "CardParts.hxx"
#include "FightersTab.hxx"
#include "FindPlayer.hxx"
#include "RowId.hxx"
#include "ShellView.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include <algorithm>
#include <cfloat>

// What the page draws: the Full card, their Fighters, their wall of awards, and the rows of the pick list and of a Sets
// list. Everything it draws was decided by player_page's functions; this only puts it on the screen.
namespace sf4e { namespace ui {
using namespace player_page;
namespace {
namespace aw = netplay::awards;

// Their wall, read only: their Panel card and the focused family's words on the left, and a tile for each family but
// Friends (the rows) beside it, each at the level their marks reach. Narrow, the card and the words go on top.
void DrawWall(const Shown& shown, const player_card::Subject& subject, FighterArt art, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action,
    bool changed) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 8 * s;
    auto* d = ImGui::GetWindowDrawList();
    const bool narrow = v2::Narrow(ImGui::GetIO().DisplaySize.x, s);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    v2::Box area{origin, ImVec2(origin.x + width, FLT_MAX)};
    v2::Box side = narrow ? area : v2::CutLeft(area, (std::min)(440 * s, width * .4f), 24 * s);
    // Beside the wall the column stays where the wall's scroll has left the top of the body.
    if (!narrow) side.min.y = (std::max)(origin.y, ImGui::GetWindowPos().y);
    const float sideTop = side.min.y;
    const auto panel = PanelOf(subject, art);
    DrawOtherCard(d, v2::CutTop(side, OtherCardHeight(panel, side.W()), 16 * s), panel);
    const auto focused = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == nav.Focus(); });
    if (focused != rows.end()) {
        const v2::Box words{side.min, ImVec2(side.max.x, side.min.y + v2::WordsHeight(side.W(), focused->detail))};
        v2::Words(d, words, focused->label, focused->detail, true);
        v2::CutTop(side, words.H(), 16 * s);
    }
    if (narrow) area.min.y = side.min.y + 8 * s;
    const float sideHeight = side.min.y - sideTop;

    const auto counters = shown.card ? aw::CountersOf(*shown.card) : aw::Counters();
    const bool counted = shown.page == Page::Card;
    const int columns = narrow ? 3 : 4;
    const float tileWidth = (area.W() - gap * (columns - 1)) / columns, tileHeight = 156 * s;
    int column = 0;
    float y = area.min.y, bottom = y;
    for (const auto& e : rows) {
        if (!HasPrefix(e.id, "award:")) continue;
        const auto* family = aw::Find(e.id.substr(6));
        if (!family) continue;
        const ImVec2 at(area.min.x + column * (tileWidth + gap), y);
        const v2::Box tile{at, ImVec2(at.x + tileWidth, at.y + tileHeight)};
        bottom = tile.max.y;
        if (++column == columns) { column = 0; y += tileHeight + gap; }
        const bool hovered = PressEntry(nav, rows, e, tile, action, changed).hovered;
        awards_wall::Tile one;
        one.family = family;
        one.level = aw::LevelOf(counters, *family);
        one.next = aw::Next(counters, *family);
        one.name = e.label;
        one.counted = counted;
        awards_wall::DrawTile(d, tile, one, e.id == nav.Focus(), hovered);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, (std::max)(bottom - origin.y, sideHeight) + gap));
}
}

void PlayerPage::Draw(const ShellView& v, const std::string& screen, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height) {
    const bool changed = nav.Focus() != focus_;
    focus_ = nav.Focus();
    const FighterArt art = v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait;
    const Shown shown = ShownNow(v);
    const auto standing = StandingNow(v, shown, PlainLinks(v, shown));
    if (screen == CardScreen) {
        const auto view = player_card::FullOf(SubjectNow(v, shown, standing), CardSize());
        DrawFullCard(view, art, rows, nav, action, changed, height);
    } else if (screen == FightersScreen) {
        const auto own = shown.card ? fighters_tab::LevelsOfCard(*shown.card) : netplay::levels::Own();
        fighters_tab::Shown tab;
        tab.levels = &own;
        tab.main = shown.card && shown.card->hasMain ? shown.card->main : -1;
        tab.art = art;
        tab.alpha = shown.page == Page::Reading ? .4f : 1.f;
        tab.others = true;
        fighters_tab::Draw(tab, rows, nav, action, changed);
    } else if (screen == AwardsScreen) {
        DrawWall(shown, SubjectNow(v, shown, standing), art, rows, nav, action, changed);
    }
}

bool PlayerPage::DrawCard(const ShellView& v, const std::string& screen, const MenuEntry& e, ImVec2 min, ImVec2 max) const {
    if (sets_.IsList(screen)) return sets_.DrawCard(v, e, min, max);
    if (screen != PickScreen) return false;
    const auto index = RowKey<std::size_t>(e.id, HitPrefix);
    if (!index || *index >= v.profile.hits.size()) return false;
    // One player of the pick list: their main, flag, class and name, and since when they have been on Ember.
    const auto& hit = v.profile.hits[*index];
    find_player::Pick pick;
    pick.name = hit.name; pick.country = hit.country; pick.badge = hit.badge; pick.fighter = hit.hasMain ? hit.main : -1;
    pick.detail = e.detail;
    pick.characterArt = v.preferences.roomCharacterPictures;
    find_player::DrawPick(ImGui::GetWindowDrawList(), pick, min, max);
    return true;
}

void PlayerPage::DrawDetail(const ShellView& v, const std::string& id, MenuNavigation& nav, const std::vector<MenuEntry>& rows) const {
    if (ArchiveTab::IsRow(id)) sets_.DrawDetail(v, id, nav, rows);
}

} }
