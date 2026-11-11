#include "PlayerCard.hxx"
#include "MenuRows.hxx"
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

Groups Grouped(const Own& own, int main) {
    Groups groups;
    groups.played = PlayedFighters(own);
    for (const int fighter : selection::RosterDisplayOrder)
        if (!own.fighters[fighter].points) groups.unplayed.push_back(fighter);
    // The main leads its own group, wherever its points would put it.
    for (auto* group : {&groups.played, &groups.unplayed}) {
        const auto at = std::find(group->begin(), group->end(), main);
        if (at != group->end()) std::rotate(group->begin(), at, at + 1);
    }
    return groups;
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
    view.main = view.account = {};
    view.facts = {};
    const bool hasMain = main >= 0 && main < static_cast<int>(Own{}.fighters.size());
    view.mainFighter = hasMain ? main : -1;
    // Whatever the answer, the card shows the main, at 0 points when there are none.
    if (!answer.loaded) {
        // A read that failed with nothing before it has no points to show.
        view.levels = answer.loading || answer.error.empty() ? CardLevels::Pending : CardLevels::Unreachable;
        if (hasMain) view.fighters.push_back({main, 0, 0, 0});
        return;
    }
    const Own& own = answer.value;
    view.levels = own.kind == Own::Kind::NoAccount ? CardLevels::NoAccount : own.kind == Own::Kind::Counted ? CardLevels::Counted : CardLevels::Unavailable;
    if (view.levels != CardLevels::Counted) {
        if (hasMain) view.fighters.push_back({main, 0, 0, 0});
        return;
    }
    view.account = own.account;
    if (hasMain) view.main = own.fighters[main];
    const auto add = [&](int fighter) { view.fighters.push_back({fighter, own.fighters[fighter].points, own.fighters[fighter].tier, own.fighters[fighter].games}); };
    if (hasMain) add(main);
    for (const int fighter : PlayedFighters(own)) {
        if (view.fighters.size() >= 3) break;
        if (fighter != main) add(fighter);
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
    case CardLevels::Unreachable: return "level.unreachable";
    case CardLevels::Counted: return card.facts.fightersPlayed ? nullptr : "level.none";
    }
    return nullptr;
}

Progress ProgressOf(const std::string& name, const Level& level, bool account) {
    Progress row;
    const bool started = level.points > 0;
    row.label = started ? name + " \xC2\xB7 " + v2::TierName(v2::FrameTier(level.tier)) : name;
    row.part = Part(level);
    if (level.next > level.points && (started || account)) row.words = loc::Tf("level.points_to", level.next - level.points, v2::TierName(v2::FrameTier(level.tier + 1)));
    else row.words = loc::T(started ? "level.top" : "level.not_played");
    return row;
}

std::vector<Progress> ProgressRows(const PlayerCardView& view) {
    std::vector<Progress> rows;
    if (view.mainFighter >= 0) rows.push_back(ProgressOf(selection::FindFighter(view.mainFighter)->name, view.main, false));
    rows.push_back(ProgressOf(loc::T("card.account"), view.account, true));
    return rows;
}

int RequestsWaiting(const netplay::friends::View& view) {
    const auto& state = view.state.value;
    return state && state->kind == netplay::friends::State::Kind::Listed ? static_cast<int>(state->incoming.size()) : 0;
}
}

void DrawFighterEdge(ImDrawList* d, v2::Box box, std::uint64_t points, int tier) {
    const float s = Scale();
    d->AddRect(ImVec2(box.min.x + s, box.min.y + s), ImVec2(box.max.x - s, box.max.y - s), points ? v2::TierColor(v2::FrameTier(tier)).stripe : v2::Colors().border, 0, 0, 2 * s);
}

namespace {
float TextAcross(const std::string& text, float size, ImFont* font) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; }

// The card's sizes, from the body: full (1920x1080), compact (a body under 620
// logical px tall: 1280x720) and narrow (under 900 logical px wide), each in
// physical px.
struct Sizes {
    bool narrow = false, compact = false;
    float hero = 0, gutter = 0, digits = 0, progress = 0, tileWidth = 0, tileHeight = 0;
    // The rows of the words column: the title row and the space under it, the name, the about line, a heading, and the space between parts.
    float titleLine = 0, titleGap = 0, nameRow = 0, aboutRow = 0, heading = 0, space = 0;
};
Sizes SizesOf(bool narrow, float height) {
    const float s = Scale();
    Sizes z;
    z.narrow = narrow;
    z.compact = !narrow && height < 620 * s;
    z.hero = (narrow ? 200 : z.compact ? 300 : 420) * s;
    z.gutter = (z.compact ? 24 : 32) * s;
    z.digits = (narrow ? 22 : z.compact ? 24 : 30) * s;
    z.progress = (narrow ? 52 : z.compact ? 32 : 40) * s;
    z.tileWidth = (z.compact ? 112 : 150) * s;
    z.tileHeight = (z.compact ? 160 : 202) * s;
    z.titleLine = (z.compact ? 30 : 34) * s;
    z.titleGap = (z.compact ? 6 : 10) * s;
    z.nameRow = (z.compact ? 40 : 44) * s;
    z.aboutRow = (z.compact ? 20 : 22) * s;
    z.heading = (z.compact ? 22 : 24) * s;
    z.space = (z.compact ? 12 : 14) * s;
    return z;
}

// The row `id` among `rows` as a button in `box`, pressed through PressEntry.
// With `glyph` (a pad button's name) the label goes after that button's glyph.
// False when there is no such row.
bool ButtonRow(ImDrawList* d, v2::Box box, const char* id, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed,
    v2::ButtonKind kind, const char* glyph = nullptr) {
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; });
    if (row == rows.end()) return false;
    const auto& c = v2::Colors();
    const float s = Scale();
    const bool hovered = PressEntry(nav, rows, *row, box, action, changed).hovered, focused = row->id == nav.Focus();
    if (!glyph) {
        v2::Button(d, box, row->label, kind, focused, hovered, row->enabled);
        return true;
    }
    v2::Button(d, box, std::string(), kind, focused, hovered, row->enabled);
    const float side = 26 * s, font = 17 * s, whole = side + 10 * s + TextAcross(row->label, font, HeadingFont()), left = box.min.x + (box.W() - whole) * .5f;
    DrawPromptGlyph(d, glyph, ImVec2(left, box.min.y + (box.H() - side) * .5f), side, MenuArt(), s);
    d->AddText(HeadingFont(), font, ImVec2(left + side + 10 * s, box.min.y + (box.H() - font) * .5f), row->enabled ? c.text : c.muted, row->label.c_str());
    return true;
}

// The main large on its tier edge, a shade across its foot with "Main" and its
// name and no number; with no main, the ground and the way to choose one.
void DrawHero(ImDrawList* d, v2::Box hero, const PlayerCardView& card, FighterArt art, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed) {
    const auto& c = v2::Colors();
    const float s = Scale();
    if (card.mainFighter < 0) {
        d->AddRectFilled(hero.min, hero.max, c.frame);
        DrawFighterEdge(d, hero, 0, 0);
        const float w = (std::min)(hero.W() - 32 * s, 280 * s), h = 48 * s, left = hero.min.x + (hero.W() - w) * .5f, top = hero.min.y + (hero.H() - h) * .5f;
        ButtonRow(d, {ImVec2(left, top), ImVec2(left + w, top + h)}, ChooseMainRow, rows, nav, action, changed, v2::ButtonKind::Primary);
        return;
    }
    DrawCharacterPortrait(FighterPicture{card.mainFighter, art, 0, 0}, hero.min, hero.max, c.frame);
    const v2::Box foot{ImVec2(hero.min.x, hero.max.y - 56 * s), hero.max};
    d->AddRectFilled(foot.min, foot.max, v2::color::Alpha(c.ink, .82f));
    v2::Text(d, ImVec2(foot.min.x + 14 * s, foot.min.y + 8 * s), foot.W() - 28 * s, loc::T("fighters.main"), 12 * s, c.muted, HeadingFont());
    v2::Text(d, ImVec2(foot.min.x + 14 * s, foot.min.y + 26 * s), foot.W() - 28 * s, selection::FindFighter(card.mainFighter)->name, 18 * s, c.text, HeadingFont());
    DrawFighterEdge(d, hero, card.main.points, card.main.tier);
}

// The title row (the worn title on its plate, the worn icon at the end; left
// out with neither), the name with the flag before it and the class emblem
// after, and the line about the player. Returns the y under them.
float DrawIdentity(ImDrawList* d, float x, float y, float room, const PlayerCardView& card, const Sizes& z) {
    const auto& c = v2::Colors();
    const float s = Scale(), line = z.titleLine, nameRow = z.nameRow, nameSize = 36 * s;
    const auto& tag = card.profile;
    if (!tag.title.empty() || tag.icon >= 0) {
        const float icon = tag.icon >= 0 ? 40 * s : 0;
        if (!tag.title.empty()) { NoteUserText(tag.title); v2::TitlePlate(d, ImVec2(x, y), room - (icon > 0 ? icon + 12 * s : 0), line, tag.title, tag.tier); }
        if (icon > 0) {
            const ImVec2 at(x + room - icon, y + (line - icon) * .5f);
            const ImU32 ink = v2::TierColor(tag.tier).stripe;
            d->AddRectFilled(at, ImVec2(at.x + icon, at.y + icon), c.frame, 4 * s);
            d->AddRect(at, ImVec2(at.x + icon, at.y + icon), ink, 4 * s, 0, 2 * s);
            v2::Icon(d, tag.icon, ImVec2(at.x + 6 * s, at.y + 6 * s), icon - 12 * s, ink);
        }
        y += line + z.titleGap;
    }
    NameMarks flag;
    flag.country = card.country;
    const float top = y + (nameRow - nameSize) * .5f, side = 32 * s;
    const float marks = DrawNameMarks(d, x, top, nameSize, flag).width;
    const SkillBadge& badge = tag.marks.badge;
    const float emblem = badge.HasClass() ? BadgeWidth(badge, side, ImGui::GetFont()) : 0;
    NoteUserText(card.name);
    const std::string name = v2::Fit(card.name, room - marks - (emblem > 0 ? emblem + 12 * s : 0), nameSize, HeadingFont(), false);
    d->AddText(HeadingFont(), nameSize, ImVec2(x + marks, top), c.text, name.c_str());
    if (emblem > 0) DrawBadge(d, ImVec2(x + marks + TextAcross(name, nameSize, HeadingFont()) + 12 * s, y + (nameRow - side) * .5f), side, ImGui::GetFont(), badge);
    y += nameRow;
    if (!tag.about.empty()) { v2::Text(d, ImVec2(x, y), room, tag.about, 15 * s, c.muted); y += z.aboutRow; }
    return y;
}

// "PROGRESS" and a row each for the main and the account: its name and frame,
// the meter, and the way to the next frame. Narrow, the words go at the right
// of the name and the meter under both. The words wrap in their column rather
// than be cut (they are long in some languages), and the row grows by the
// lines. Returns the y under them.
float DrawProgress(ImDrawList* d, float x, float y, float room, const std::vector<player_card::Progress>& rows, const Sizes& z) {
    const auto& c = v2::Colors();
    const float s = Scale(), size = 14 * s;
    v2::SectionHeading(d, ImVec2(x, y), loc::T("card.progress"));
    y += z.heading;
    for (const auto& row : rows) {
        const float natural = TextAcross(row.words, size, ImGui::GetFont()) + s;
        if (z.narrow) {
            const float words = (std::min)(natural, room * .5f), height = v2::Paragraph(nullptr, ImVec2(), words, row.words, size, 0);
            const float meter = (std::max)(30 * s, height + 2 * s);
            v2::Text(d, ImVec2(x, y), room - words - 12 * s, row.label, 15 * s, c.text, HeadingFont());
            v2::Paragraph(d, ImVec2(x + room - words, y - 2 * s), words, row.words, size, c.muted);
            v2::Meter(d, {ImVec2(x, y + meter), ImVec2(x + room, y + meter + 10 * s)}, row.part, 0.f);
            y += meter + 22 * s;
        } else {
            const float label = 200 * s, words = 220 * s, shown = (std::min)(natural, words), height = v2::Paragraph(nullptr, ImVec2(), shown, row.words, size, 0);
            const float line = (std::max)(z.progress, height + 8 * s), mid = y + (line - 10 * s) * .5f;
            v2::Text(d, ImVec2(x, y + (line - 15 * s) * .5f), label, row.label, 15 * s, c.text, HeadingFont());
            v2::Meter(d, {ImVec2(x + label + 12 * s, mid), ImVec2(x + room - words - 12 * s, mid + 10 * s)}, row.part, 0.f);
            v2::Paragraph(d, ImVec2(x + room - shown, y + (line - height) * .5f), shown, row.words, size, c.muted);
            y += line;
        }
    }
    return y;
}

// One fighter of the strip: its picture on its tier edge with its points on
// it, and under it the name and games.
void DrawStripTile(ImDrawList* d, v2::Box box, const CardFighter& fighter, FighterArt art, bool focused, bool hovered, float alpha) {
    const auto& c = v2::Colors();
    const float s = Scale(), pad = 8 * s, side = box.W() - 2 * pad;
    v2::CardGround(d, box, focused, hovered);
    const v2::Box tile{ImVec2(box.min.x + pad, box.min.y + pad), ImVec2(box.max.x - pad, box.min.y + pad + side)};
    DrawCharacterPortrait(FighterPicture{fighter.fighter, art, 0, 0}, tile.min, tile.max, c.frame);
    v2::LevelBand(d, tile, fighter.points, alpha);
    DrawFighterEdge(d, tile, fighter.points, fighter.tier);
    const float y = tile.max.y + 6 * s;
    v2::Text(d, ImVec2(box.min.x + pad, y), side, selection::FindFighter(fighter.fighter)->name, 14 * s, c.text, HeadingFont());
    v2::TextShrunk(d, ImVec2(box.min.x + pad, y + 20 * s), side, player_card::GamesWords(fighter.games), 12 * s, 9 * s, c.muted);
    if (focused) v2::Focus(d, box.min, box.max);
}

// "TOP FIGHTERS" and the strip, each tile a row whose Select opens the Fighters
// tab on it. Narrow, the tiles share the width. Returns the y under them.
float DrawStrip(ImDrawList* d, float x, float y, float room, const PlayerCardView& card, const Sizes& z, FighterArt art, float alpha,
    const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed) {
    const float s = Scale(), between = 12 * s, width = z.narrow ? (room - 2 * between) / 3 : z.tileWidth, height = z.narrow ? width + 42 * s : z.tileHeight;
    bool headed = false;
    int shown = 0;
    for (const auto& fighter : card.fighters) {
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == CardFighterRow(fighter.fighter); });
        if (row == rows.end()) continue;
        if (!headed) { v2::SectionHeading(d, ImVec2(x, y), loc::T("card.top_fighters")); y += z.heading; headed = true; }
        const float left = x + shown++ * (width + between);
        const v2::Box box{ImVec2(left, y), ImVec2(left + width, y + height)};
        const bool hovered = PressEntry(nav, rows, *row, box, action, changed).hovered;
        DrawStripTile(d, box, fighter, art, row->id == nav.Focus(), hovered, alpha);
    }
    return shown ? y + height : y;
}
}


// Connect Discord, the way to points and friends a player without an account has.
MenuEntry ConnectDiscordRow() {
    auto connect = Row(DiscordConnectScreen, loc::T("screen.connect_discord"), loc::T("rankings.connect_detail"));
    connect.value = loc::T("identity.discord_none");
    connect.hint = loc::T("screen.connect_discord");
    return connect;
}

float DrawRowButton(const char* id, ImVec2 at, float width, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed) {
    const float s = Scale();
    const v2::Box box{ImVec2(at.x, at.y + 8 * s), ImVec2(at.x + width, at.y + 8 * s + 48 * s)};
    return ButtonRow(ImGui::GetWindowDrawList(), box, id, rows, nav, action, changed, v2::ButtonKind::Primary) ? box.max.y : at.y;
}

float DrawCardTab(const ShellView& v, const PlayerCardView& card, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed, float height) {
    const auto& c = v2::Colors();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 16 * s;
    auto* d = ImGui::GetWindowDrawList();
    const Sizes z = SizesOf(v2::Narrow(ImGui::GetIO().DisplaySize.x, s), height);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const FighterArt art = v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait;
    // A read still going dims the numbers.
    const float alpha = card.levels == CardLevels::Pending ? .4f : 1.f;
    const v2::Box hero{origin, ImVec2(origin.x + z.hero, origin.y + z.hero)};
    DrawHero(d, hero, card, art, rows, nav, action, changed);

    // Wide: Edit card under the hero and the card's words in a column to its
    // right. Narrow: the words beside the hero, or under it when too tight.
    const bool beside = z.narrow && width >= 560 * s;
    float x = hero.max.x + (z.narrow ? 16 * s : z.gutter), y = origin.y, bottom = hero.max.y;
    float room = origin.x + width - x;
    if (!z.narrow) {
        room = (std::min)(room, z.compact ? FLT_MAX : 960 * s);
        const v2::Box edit{ImVec2(hero.min.x, hero.max.y + gap), ImVec2(hero.max.x, hero.max.y + gap + 48 * s)};
        ButtonRow(d, edit, CardEditRow, rows, nav, action, changed, v2::ButtonKind::Secondary, "Y");
        bottom = edit.max.y;
    } else if (!beside) {
        x = origin.x; room = width; y = hero.max.y + 12 * s;
    }
    y = DrawIdentity(d, x, y, room, card, z) + 8 * s;
    const std::vector<v2::TagView::Fact> facts{{loc::T("card.counted_games"), std::to_string(card.facts.games)}, {loc::T("card.win_rate"), std::to_string(card.facts.winPercent) + "%"},
        {loc::T("card.fighters_played"), loc::Tf("card.of", card.facts.fightersPlayed, selection::FighterCount)}};
    const float lines = v2::LevelLines(d, ImVec2(x, y), z.digits, 8 * s, card.main.points, card.account.points, alpha);
    if (z.compact) {
        // The facts stand beside the points lines.
        const float left = 260 * s;
        y += (std::max)(lines, v2::FactCells(d, ImVec2(x + left, y), room - left, facts, 3));
    } else y += lines;
    y += z.space;
    if (z.narrow) y = (std::max)(y, hero.max.y + z.space);
    y = DrawProgress(d, x, y, room, player_card::ProgressRows(card), z) + z.space;
    if (!z.compact) y += v2::FactCells(d, ImVec2(x, y), room, facts, z.narrow ? 2 : 3) + z.space;
    if (const char* why = player_card::NoLevelWords(card)) y += v2::Paragraph(d, ImVec2(x, y), room, loc::T(why), 14 * s, c.muted) + z.space;
    y = DrawStrip(d, x, y, room, card, z, art, alpha, rows, nav, action, changed);
    y = DrawRowButton(DiscordConnectScreen, ImVec2(x, y), (std::min)(room, 360 * s), rows, nav, action, changed);
    if (z.narrow) {
        const v2::Box edit{ImVec2(origin.x, y + gap), ImVec2(origin.x + width, y + gap + 48 * s)};
        if (ButtonRow(d, edit, CardEditRow, rows, nav, action, changed, v2::ButtonKind::Secondary, "Y")) y = edit.max.y;
    }
    const float total = (std::max)(y, bottom) + gap - origin.y;
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, total));
    return total;
}

float DrawNameLine(ImDrawList* d, float x, float y, float room, float row, float size, const std::string& name, CountryCode country, const SkillBadge& badge) {
    const float s = Scale(), side = size * 24.f / 22.f;
    NameMarks flag;
    flag.country = country;
    const float top = y + (row - size) * .5f, marks = DrawNameMarks(d, x, top, size, flag).width;
    const float emblem = badge.HasClass() ? BadgeWidth(badge, side, ImGui::GetFont()) : 0;
    NoteUserText(name);
    const std::string shown = v2::Fit(name, room - marks - (emblem > 0 ? emblem + 8 * s : 0), size, HeadingFont(), false);
    d->AddText(HeadingFont(), size, ImVec2(x + marks, top), v2::Colors().text, shown.c_str());
    if (emblem > 0) DrawBadge(d, ImVec2(x + marks + TextAcross(shown, size, HeadingFont()) + 8 * s, y + (row - side) * .5f), side, ImGui::GetFont(), badge);
    return row;
}

void DrawOtherCard(ImDrawList* d, v2::Box box, const OtherCardView& card) {
    const auto& c = v2::Colors();
    const float s = Scale(), pad = 14 * s, side = (std::min)(box.H() - 2 * pad, 128 * s);
    d->AddRectFilled(box.min, box.max, v2::PanelGround(), 4 * s);
    d->AddRect(box.min, box.max, c.border, 4 * s, 0, s);
    const v2::Box tile{ImVec2(box.min.x + pad, box.min.y + pad), ImVec2(box.min.x + pad + side, box.min.y + pad + side)};
    if (card.fighter >= 0) DrawCharacterPortrait(FighterPicture{card.fighter, card.art, 0, 0}, tile.min, tile.max, c.frame);
    else d->AddRectFilled(tile.min, tile.max, c.frame);
    DrawFighterEdge(d, tile, card.points ? card.fighterPoints : 0, card.fighterTier);
    // How the viewer stands with the player is a chip at the card's top right.
    const float x = tile.max.x + 16 * s, right = box.max.x - pad;
    float y = box.min.y + pad, room = right - x;
    if (!card.chip.empty()) room -= v2::ChipWidth(card.chip) + 12 * s;
    y += DrawNameLine(d, x, y, room, 30 * s, 22 * s, card.name, card.country, card.badge) + 6 * s;
    if (!card.chip.empty()) v2::Chip(d, ImVec2(right - v2::ChipWidth(card.chip), box.min.y + pad), card.chip, card.chipTone);
    if (!card.title.empty()) {
        NoteUserText(card.title);
        v2::TitlePlate(d, ImVec2(x, y), right - x, 28 * s, card.title, card.titleTier);
        y += 34 * s;
    }
    if (!card.points) return;
    // The digits of the design (26 / 22 px), shrunk where the card is short.
    const float digits = (std::min)((ImGui::GetIO().DisplaySize.y / s <= 720 ? 22 : 26) * s, (box.max.y - pad - y - 4 * s) * (card.accountKnown ? .5f : 1.f));
    if (digits < 10 * s) return;
    // A friend's card carries the main's points and no account's: one line.
    if (card.accountKnown) v2::LevelLines(d, ImVec2(x, y), digits, 8 * s, card.fighterPoints, card.accountPoints);
    else v2::Level(d, ImVec2(x, y), digits, card.fighterPoints, "points.label");
}

OtherCardView OwnAsOthers(const PlayerCardView& card) {
    OtherCardView other;
    other.name = card.name;
    other.country = card.country;
    other.badge = card.profile.marks.badge;
    other.title = card.profile.title;
    other.titleTier = card.profile.tier;
    other.fighter = card.mainFighter;
    other.fighterPoints = card.main.points;
    other.fighterTier = card.main.tier;
    other.accountPoints = card.account.points;
    return other;
}
} }
