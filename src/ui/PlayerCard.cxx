#include "PlayerCard.hxx"
#include "ShellView.hxx"
#include "SignInSites.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <string>

namespace sf4e { namespace ui {
namespace player_card {
std::vector<int> PlayedFighters(const Own& own) {
    std::vector<int> played;
    for (int fighter = 0; fighter < static_cast<int>(own.fighters.size()); ++fighter)
        if (own.fighters[fighter].points) played.push_back(fighter);
    std::sort(played.begin(), played.end(), [&](int a, int b) {
        const auto &x = own.fighters[a], &y = own.fighters[b];
        return x.points != y.points ? x.points > y.points : a < b;
    });
    return played;
}

float Part(const Level& level) {
    // A fighter with no points has no tier from the bridge yet: an empty meter, not the top.
    if (!level.points && !level.next) return 0.f;
    if (!level.next || level.next <= level.floor) return 1.f;
    const std::uint64_t done = (std::max)(level.points, level.floor) - level.floor;
    return (std::min)(1.f, static_cast<float>(done) / static_cast<float>(level.next - level.floor));
}

void Fill(PlayerCardView& view, const netplay::Answered<Own>& answer, int main) {
    view.fighters.clear();
    view.account = 0;
    view.progress = {};
    view.facts = {};
    const bool hasMain = main >= 0 && main < static_cast<int>(Own{}.fighters.size());
    // Whatever the answer, the card shows the main, at 0 points when there are none.
    if (!answer.loaded) {
        // A read that failed with nothing before it has no points to show.
        view.levels = answer.loading || answer.error.empty() ? CardLevels::Pending : CardLevels::Unavailable;
        if (hasMain) view.fighters.push_back({main, 0, 0, 0});
        return;
    }
    const Own& own = answer.value;
    view.levels = own.kind == Own::Kind::NoAccount ? CardLevels::NoAccount : own.kind == Own::Kind::Counted ? CardLevels::Counted : CardLevels::Unavailable;
    if (view.levels != CardLevels::Counted) {
        if (hasMain) view.fighters.push_back({main, 0, 0, 0});
        return;
    }
    view.account = own.account.points;
    const auto add = [&](int fighter) { view.fighters.push_back({fighter, own.fighters[fighter].points, own.fighters[fighter].tier, own.fighters[fighter].games}); };
    if (hasMain) add(main);
    for (const int fighter : PlayedFighters(own)) {
        if (view.fighters.size() >= 3) break;
        if (fighter != main) add(fighter);
    }
    if (hasMain && own.fighters[main].points) {
        const auto& level = own.fighters[main];
        view.progress = {level.points, level.tier, level.next};
    }
    std::uint64_t wins = 0;
    for (const auto& level : own.fighters) {
        view.facts.games += level.games;
        wins += level.wins;
        view.facts.fightersPlayed += level.points ? 1 : 0;
    }
    view.facts.winPercent = view.facts.games ? static_cast<int>((wins * 100 + view.facts.games / 2) / view.facts.games) : 0;
}

std::string GamesWords(std::uint64_t games) { return loc::Tf(games == 1 ? "level.game_one" : "level.games", games); }

const char* NoLevelWords(const PlayerCardView& card) {
    switch (card.levels) {
    case CardLevels::Pending: return "level.loading";
    case CardLevels::NoAccount: return "level.no_account";
    case CardLevels::Unavailable: return "level.unavailable";
    case CardLevels::Counted: return card.facts.fightersPlayed ? nullptr : "level.none";
    }
    return nullptr;
}
}

namespace {
// The points plate and the class plate across the hero's foot (PLAYER_CARD.md,
// Card tab): the main's points with its name and games at the left, the class
// emblem at the right. A main without points shows 0; why it has none is said
// under the card's facts.
void DrawPlates(ImDrawList* d, v2::Box foot, const PlayerCardView& card, int main) {
    const auto& c = v2::Colors();
    const float s = Scale(), pad = 12 * s, plate = foot.H() - 2 * pad;
    float right = foot.max.x - pad;
    const auto& badge = card.profile.marks.badge;
    if (badge.HasClass()) {
        const float side = 28 * s, width = BadgeWidth(badge, side, ImGui::GetFont());
        right -= width;
        d->AddText(HeadingFont(), 12 * s, ImVec2(right, foot.min.y + pad), c.muted, loc::T("card.class"));
        DrawBadge(d, ImVec2(right, foot.min.y + pad + 18 * s), side, ImGui::GetFont(), badge);
        right -= pad;
    }
    const float left = foot.min.x + pad, room = right - left;
    const auto* fighter = main >= 0 ? selection::FindFighter(main) : nullptr;
    const bool leads = !card.fighters.empty() && card.fighters.front().fighter == main;
    const std::uint64_t points = leads ? card.fighters.front().points : 0, games = leads ? card.fighters.front().games : 0;
    const float width = v2::Level(d, ImVec2(left, foot.min.y + pad), plate, points, leads ? card.fighters.front().tier : 0), x = left + width + pad,
                text = (std::max)(0.f, room - width - pad);
    if (fighter) {
        v2::Text(d, ImVec2(x, foot.min.y + pad + 4 * s), text, fighter->name, 18 * s, c.text, HeadingFont());
        v2::Text(d, ImVec2(x, foot.min.y + pad + 30 * s), text, player_card::GamesWords(games), 14 * s, c.muted);
    } else {
        v2::Paragraph(d, ImVec2(x, foot.min.y + pad), text, loc::T("profile.choose_main_detail"), 14 * s, c.muted);
    }
}

// One fighter of the strip: its picture with its points on it, and under it the name and games.
void DrawStripTile(ImDrawList* d, v2::Box box, const CardFighter& fighter, FighterArt art, bool focused, bool hovered) {
    const auto& c = v2::Colors();
    const float s = Scale(), pad = 8 * s, side = box.W() - 2 * pad;
    v2::CardGround(d, box, focused, hovered);
    const v2::Box tile{ImVec2(box.min.x + pad, box.min.y + pad), ImVec2(box.max.x - pad, box.min.y + pad + side)};
    DrawCharacterPortrait(FighterPicture{fighter.fighter, art, 0, 0}, tile.min, tile.max, c.frame);
    v2::LevelBand(d, tile, fighter.points, fighter.tier);
    const float y = tile.max.y + 6 * s;
    v2::Text(d, ImVec2(box.min.x + pad, y), side, selection::FindFighter(fighter.fighter)->name, 14 * s, c.text, HeadingFont());
    v2::Text(d, ImVec2(box.min.x + pad, y + 20 * s), side, player_card::GamesWords(fighter.games), 12 * s, c.muted);
    if (focused) v2::Focus(d, box.min, box.max);
}
}

float DrawRowButton(const char* id, ImVec2 at, float width, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed) {
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; });
    if (row == rows.end()) return at.y;
    const v2::Box box{ImVec2(at.x, at.y + 8 * Scale()), ImVec2(at.x + width, at.y + 8 * Scale() + 48 * Scale())};
    const bool hovered = PressEntry(nav, rows, *row, box, action, changed).hovered;
    v2::Button(ImGui::GetWindowDrawList(), box, row->label, v2::ButtonKind::Primary, row->id == nav.Focus(), hovered);
    return box.max.y;
}

float DrawCardTab(const ShellView& v, int main, const PlayerCardView& card, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed) {
    const auto& c = v2::Colors();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 16 * s, footHeight = 80 * s;
    auto* d = ImGui::GetWindowDrawList();
    const bool narrow = v2::Narrow(ImGui::GetIO().DisplaySize.x, s);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const FighterArt art = v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait;
    // The hero, the main large. Wide, the plates lie on a band across its foot
    // and the card's words stand to its right; narrow, the plates go under it
    // and the words under them.
    const float heroWidth = narrow ? width : (std::min)(width * .42f, 420 * s), heroHeight = narrow ? (std::min)(width * .62f, 300 * s) : heroWidth;
    const v2::Box hero{origin, ImVec2(origin.x + heroWidth, origin.y + heroHeight)};
    if (main >= 0) DrawCharacterPortrait(FighterPicture{main, art, 0, 0}, hero.min, hero.max, c.frame);
    else d->AddRectFilled(hero.min, hero.max, c.frame);
    d->AddRect(hero.min, hero.max, c.accent, 0, 0, 3 * s);
    v2::Box foot{ImVec2(hero.min.x, hero.max.y - footHeight), hero.max}, words{ImVec2(hero.max.x + 24 * s, origin.y), ImVec2(origin.x + width, FLT_MAX)};
    if (narrow) {
        foot = {ImVec2(origin.x, hero.max.y + gap), ImVec2(origin.x + width, hero.max.y + gap + footHeight)};
        words = {ImVec2(origin.x, foot.max.y + gap), ImVec2(origin.x + width, FLT_MAX)};
        d->AddRectFilled(foot.min, foot.max, c.frame);
    } else d->AddRectFilled(foot.min, foot.max, v2::color::Alpha(c.ink, .82f));
    DrawPlates(d, foot, card, main);
    // The words that stand for the points are a row the pad can reach too.
    if (const auto state = std::find_if(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == ProfileStateRow; }); state != rows.end()) {
        const bool hovered = PressEntry(nav, rows, *state, foot, action, changed).hovered;
        if (hovered || state->id == nav.Focus()) v2::Focus(d, foot.min, foot.max);
    }

    float y = words.min.y;
    const float x = words.min.x, room = words.W(), line = 34 * s, nameSize = 26 * s;
    if (!card.profile.title.empty()) { v2::TitlePlate(d, ImVec2(x, y), room, line, card.profile.title, card.profile.tier); y += line + 10 * s; }
    const float marks = DrawNameMarks(d, x, y + (line - nameSize) * .5f, nameSize, card.profile.marks).width;
    NoteUserText(card.name);
    d->AddText(HeadingFont(), nameSize, ImVec2(x + marks, y + (line - nameSize) * .5f), c.text, v2::Fit(card.name, room - marks, nameSize, HeadingFont(), false).c_str());
    y += line + 4 * s;
    if (!card.profile.about.empty()) { v2::Text(d, ImVec2(x, y), room, card.profile.about, 14 * s, c.muted); y += 24 * s; }
    y += 8 * s;
    const bool counted = card.levels == CardLevels::Counted;
    // The main's meter: "1986 points" and the points to the next tier, or the
    // top frame. A main without points has an empty meter and 0 points.
    if (main >= 0) {
        const player_card::Level none{};
        const auto& own = counted ? v.levels.value.fighters[main] : none;
        v2::Text(d, ImVec2(x, y), room, loc::Tf("level.value", own.points), 20 * s, c.text, HeadingFont());
        y += 30 * s;
        v2::Meter(d, {ImVec2(x, y), ImVec2(x + room, y + 10 * s)}, player_card::Part(own), 0.f);
        y += 20 * s;
        if (own.next) y += v2::Paragraph(d, ImVec2(x, y), room, loc::Tf("level.points_to", own.next - own.points, v2::TierName(v2::FrameTier(own.tier + 1))), 14 * s, c.muted) + 10 * s;
        else if (own.points) y += v2::Paragraph(d, ImVec2(x, y), room, loc::T("level.top"), 14 * s, c.muted) + 10 * s;
        else y += 4 * s;
    }
    // The facts, 0 until there are points; why there are none; and why others
    // see no points when they do not.
    const std::vector<v2::TagView::Fact> facts{{loc::T("card.counted_games"), std::to_string(card.facts.games)},
        {loc::T("card.win_rate"), std::to_string(card.facts.winPercent) + "%"},
        {loc::T("card.fighters_played"), loc::Tf("card.of", card.facts.fightersPlayed, static_cast<int>(v.levels.value.fighters.size()))}};
    y += v2::FactCells(d, ImVec2(x, y), room, facts, room < 520 * s ? 2 : 3) + 14 * s;
    if (const char* why = player_card::NoLevelWords(card)) y += v2::Paragraph(d, ImVec2(x, y), room, loc::T(why), 14 * s, c.muted) + 14 * s;
    // The strip: the main and the two highest others, each a row whose Select opens the Fighters tab.
    const float between = 12 * s, tile = (std::min)(150 * s, (room - 2 * between) / 3), tileHeight = tile + 56 * s;
    int shown = 0;
    for (const auto& fighter : card.fighters) {
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == CardFighterRow(fighter.fighter); });
        if (row == rows.end()) continue;
        const float left = x + shown++ * (tile + between);
        const v2::Box box{ImVec2(left, y), ImVec2(left + tile, y + tileHeight)};
        const bool hovered = PressEntry(nav, rows, *row, box, action, changed).hovered;
        DrawStripTile(d, box, fighter, art, row->id == nav.Focus(), hovered);
    }
    if (shown) y += tileHeight;
    y = DrawRowButton(DiscordConnectScreen, ImVec2(x, y), (std::min)(room, 360 * s), rows, nav, action, changed);
    const float height = (std::max)(y, narrow ? y : hero.max.y) + gap - origin.y;
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    return height;
}
} }
